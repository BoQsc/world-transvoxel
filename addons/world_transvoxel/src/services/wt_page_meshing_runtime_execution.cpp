#include "services/wt_page_meshing_runtime_internal.h"

#include "bake/wt_chunk_baker.h"
#include "editing/wt_chunk_edit_state.h"
#include "editing/wt_edit_surface_shift_source.h"
#include "meshing/wt_material_volume_sample_source.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_chunk_page_sample_source.h"
#include "streaming/wt_foreground_priority.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

namespace world_transvoxel {
namespace {

void record_execution_failure_key(
	WtPageMeshingRuntimeMetrics &metrics,
	const WtChunkKey &key
) noexcept {
	metrics.last_failure_key_x = key.x;
	metrics.last_failure_key_y = key.y;
	metrics.last_failure_key_z = key.z;
	metrics.last_failure_key_lod = key.lod;
}

std::uint64_t steady_time_ns() noexcept {
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()
		).count()
	);
}

std::uint8_t cumulative_dirty_regular_bricks(
	const WtChunkKey &key,
	const WtEditBounds &dirty
) noexcept {
	if (key.lod != 0) return 0xff;
	const WtGridPoint chunk_minimum = wt_chunk_bounds(key).minimum;
	std::uint8_t mask = 0;
	for (std::int32_t z = 0; z < 2; ++z) {
		for (std::int32_t y = 0; y < 2; ++y) {
			for (std::int32_t x = 0; x < 2; ++x) {
				const WtGridPoint minimum = {
					chunk_minimum.x + x * 8,
					chunk_minimum.y + y * 8,
					chunk_minimum.z + z * 8,
				};
				const WtGridPoint maximum = {
					chunk_minimum.x + (x + 1) * 8,
					chunk_minimum.y + (y + 1) * 8,
					chunk_minimum.z + (z + 1) * 8,
				};
				const bool intersects =
					dirty.maximum.x >= minimum.x && dirty.minimum.x <= maximum.x &&
					dirty.maximum.y >= minimum.y && dirty.minimum.y <= maximum.y &&
					dirty.maximum.z >= minimum.z && dirty.minimum.z <= maximum.z;
				if (intersects) mask |= static_cast<std::uint8_t>(
					1U << static_cast<unsigned int>(x + y * 2 + z * 4)
				);
			}
		}
	}
	return mask == 0 ? 0xff : mask;
}

class PointEditReplaySink final : public WtEditReplaySink {
public:
	PointEditReplaySink(
		const WtGridPoint &point,
		WtScalarSample &sample,
		const WtProceduralWorldDescriptor *procedural_descriptor
	) noexcept :
			point_(point),
			sample_(sample),
			procedural_descriptor_(procedural_descriptor) {
	}

	bool apply(const WtEditCommand &command) noexcept override {
		bool changed = false;
		return wt_apply_edit_command_to_sample(
			command, point_, sample_, changed, procedural_descriptor_
		);
	}

private:
	WtGridPoint point_;
	WtScalarSample &sample_;
	const WtProceduralWorldDescriptor *procedural_descriptor_ = nullptr;
};

class EditedProceduralSampleSource final : public WtChunkSampleSource {
public:
	EditedProceduralSampleSource(
		WtAsyncStorageService &storage,
		const WtEditJournal &journal,
		std::uint64_t source_revision,
		std::uint64_t initial_world_revision,
		std::uint64_t world_revision
	) noexcept :
			storage_(storage),
			journal_(journal),
			world_revision_(world_revision),
			valid_(storage.procedural_descriptor(procedural_descriptor_) &&
				journal.initialized() &&
				journal.source_revision() == source_revision &&
				journal.initial_world_revision() == initial_world_revision &&
				world_revision >= initial_world_revision &&
				world_revision <= journal.current_world_revision()) {
	}

	bool sample(
		const WtGridPoint &point,
		WtScalarSample &output
	) const noexcept override {
		if (!valid_ || !storage_.sample_procedural_base(point, output)) {
			return false;
		}
		PointEditReplaySink sink(point, output, &procedural_descriptor_);
		return journal_.replay_until(world_revision_, sink) ==
			WtEditJournalStatus::Ok;
	}

	bool valid() const noexcept {
		return valid_;
	}

private:
	WtAsyncStorageService &storage_;
	const WtEditJournal &journal_;
	std::uint64_t world_revision_ = 0;
	WtProceduralWorldDescriptor procedural_descriptor_;
	bool valid_ = false;
};

} // namespace

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::prepare_mesh_job(
	const WtChunkJob &job,
	WtStreamScheduler &scheduler,
	const WtEditJournal *edit_journal,
	std::uint64_t initial_world_revision,
	WtAsyncStorageService *authoritative_storage,
	const WtTerrainMeshReadyCallback &terrain_mesh_ready,
	bool visual_required,
	const WtMeshExecutionCallback &execution_callback,
	const WtMeshCellCaptureCallback &cell_capture_callback,
	bool pre_mesh_field_capture,
	bool collision_required,
	bool defer_gpu_capture,
	bool live_collision_patch_base,
	PreparedMeshJob &prepared
) {
	const std::uint64_t started = steady_time_ns();
	const auto record_time = [this, started]() {
		const std::uint64_t elapsed = steady_time_ns() - started;
		metrics_.mesh_prepare_time_ns_last = elapsed;
		metrics_.mesh_prepare_time_ns_total += elapsed;
		metrics_.mesh_prepare_time_ns_maximum = std::max(
			metrics_.mesh_prepare_time_ns_maximum,
			elapsed
		);
	};
	if (!valid_) return WtPageMeshingRuntimeStatus::InvalidConfiguration;
	if (job.stage != WtChunkJobStage::Mesh || job.generation.value == 0) {
		return WtPageMeshingRuntimeStatus::InvalidJob;
	}
	const WtChunkRecord *scheduler_record = scheduler.find_record(job.key);
	if (scheduler_record == nullptr ||
		scheduler_record->generation != job.generation ||
		scheduler_record->source_revision != job.source_revision ||
		scheduler_record->world_revision != job.world_revision ||
		scheduler_record->lifecycle != WtChunkLifecycle::Meshing) {
		return WtPageMeshingRuntimeStatus::NotReady;
	}
	auto record = find_record(job.key);
	if (record == records_.end()) return WtPageMeshingRuntimeStatus::NotFound;
	if (record->generation != job.generation ||
		record->source_revision != job.source_revision ||
		record->world_revision != job.world_revision ||
		record->phase != WtPageMeshingRuntimePhase::AwaitingMesh) {
		return WtPageMeshingRuntimeStatus::NotReady;
	}
	++metrics_.mesh_jobs;
	const std::size_t record_index = static_cast<std::size_t>(
		record - records_.begin()
	);
	const auto primary = std::lower_bound(
		record->dependencies.begin(),
		record->dependencies.end(),
		record->key,
		[](const Dependency &left, const WtChunkKey &right) {
			return left.key < right;
		}
	);
	bool source_valid = primary != record->dependencies.end() &&
		primary->key == record->key && static_cast<bool>(primary->page);
	bool incremental_edit = false;
	std::uint8_t dirty_regular_brick_mask = 0xff;
	WtEditBounds dirty_edit_bounds;
	bool has_dirty_edit_bounds = false;
	if (source_valid && edit_journal != nullptr) {
		WtProceduralWorldDescriptor procedural_descriptor;
		const WtProceduralWorldDescriptor *procedural_descriptor_pointer =
			authoritative_storage != nullptr &&
				authoritative_storage->procedural_descriptor(
					procedural_descriptor
				) ? &procedural_descriptor : nullptr;
		std::unique_ptr<EditedProceduralSampleSource> edited_source;
		if (authoritative_storage != nullptr) {
			edited_source = std::make_unique<EditedProceduralSampleSource>(
				*authoritative_storage,
				*edit_journal,
				record->source_revision,
				initial_world_revision,
				record->world_revision
			);
		}
		bool surface_shift_failure = false;
		for (Dependency &dependency : record->dependencies) {
			std::shared_ptr<const WtChunkPage> replay_base = dependency.page;
			std::uint64_t replay_base_revision = initial_world_revision;
			if (record->world_revision > initial_world_revision) {
				std::shared_ptr<const WtChunkPage> cached_page;
				std::uint64_t cached_revision = 0;
				if (find_edited_page(
						dependency.key,
						record->source_revision,
						record->world_revision,
						cached_revision,
						cached_page
					)) {
					replay_base = std::move(cached_page);
					replay_base_revision = cached_revision;
					++metrics_.edited_page_cache_hits;
				} else {
					++metrics_.edited_page_cache_misses;
				}
			}
			WtChunkEditState edit_state;
			if (!replay_base ||
				edit_state.initialize(
					*replay_base,
					record->source_revision,
					replay_base_revision,
					procedural_descriptor_pointer
				) != WtChunkEditStatus::Ok ||
				edit_journal->replay_after_until(
					replay_base_revision, record->world_revision, edit_state
				) !=
					WtEditJournalStatus::Ok ||
				edit_state.current_world_revision() != record->world_revision) {
				source_valid = false;
				break;
			}
			WtChunkPage edited_page = edit_state.page();
			if (dependency.key == record->key) {
				const WtEditBounds *cumulative_dirty =
					edit_state.surface_shift_dirty_bounds();
				incremental_edit = job.edit_delta.valid &&
					job.edit_delta.dirty_regular_brick_mask != 0 &&
					record->world_revision > initial_world_revision;
				if (incremental_edit) {
					dirty_regular_brick_mask =
						job.edit_delta.dirty_regular_brick_mask;
					dirty_edit_bounds = {
						job.edit_delta.dirty_minimum,
						job.edit_delta.dirty_maximum,
					};
					has_dirty_edit_bounds = true;
					if (cumulative_dirty != nullptr &&
						cumulative_dirty_regular_bricks(record->key, *cumulative_dirty) !=
							dirty_regular_brick_mask) {
						++metrics_.cumulative_dirty_mask_avoided;
					}
				}
			}
			if (!edited_page.surface_shift_valid) {
				const WtChunkPageSampleSource retained_source(*replay_base);
				const WtEditSurfaceShiftSource local_source(
					edited_source ? static_cast<const WtChunkSampleSource &>(*edited_source) : retained_source,
					*replay_base, edit_state.surface_shift_dirty_bounds());
				if (!edited_source || !edited_source->valid() ||
					wt_build_surface_shift_records(
						edited_page,
						local_source,
						preparation_scratch_.multiresolution
					) != WtSurfaceShiftBuildStatus::Ok) {
					source_valid = false;
					surface_shift_failure = true;
					++metrics_.surface_shift_failures;
					break;
				}
				++metrics_.surface_shift_rebuilds;
			}
			dependency.page = std::make_shared<const WtChunkPage>(
				std::move(edited_page)
			);
			if (record->world_revision > initial_world_revision) {
				store_edited_page(
					dependency.key,
					record->source_revision,
					record->world_revision,
					dependency.page
				);
			}
		}
		if (!source_valid) {
			for (Dependency &dependency : record->dependencies) {
				dependency.page.reset();
			}
			record->phase = WtPageMeshingRuntimePhase::MeshFailedReady;
			++metrics_.mesh_failures;
			record_execution_failure_key(metrics_, record->key);
			submit_pending_result(record_index, scheduler);
			record_time();
			return surface_shift_failure ?
				WtPageMeshingRuntimeStatus::SurfaceShiftFailure :
				WtPageMeshingRuntimeStatus::EditReplayFailure;
		}
	}
	prepared.job = job;
	prepared.transition_mask = record->transition_mask;
	prepared.cached_transition_mask = record->cached_transition_mask;
	prepared.visual_required = visual_required;
	prepared.collision_required = collision_required;
	// Visual and collision viewers are submitted independently. A LOD0 GPU
	// visual job can otherwise leave the worker before the matching collision
	// demand is merged, forcing a second full page traversal several frames
	// later. LOD0 is the bounded near-player working set, so derive and cache its
	// eight collision blocks during the existing immutable-page traversal.
	prepared.gpu_lod0_collision_prewarm = pre_mesh_field_capture &&
		visual_required && job.key.lod == 0 &&
		job.priority >= kWtInteractionFocusPriority;
	prepared.terrain_mesh_ready = terrain_mesh_ready;
	prepared.execution_callback = execution_callback;
	prepared.cell_capture_callback = cell_capture_callback;
	prepared.pre_mesh_field_capture = pre_mesh_field_capture;
	record->pre_mesh_field_capture = pre_mesh_field_capture;
	prepared.defer_gpu_capture = defer_gpu_capture;
	prepared.live_collision_patch_base = live_collision_patch_base;
	prepared.incremental_edit = incremental_edit;
	prepared.dirty_regular_brick_mask = dirty_regular_brick_mask;
	prepared.dirty_edit_bounds = dirty_edit_bounds;
	prepared.has_dirty_edit_bounds = has_dirty_edit_bounds;
	prepared.gpu_resident_visual_only = pre_mesh_field_capture &&
		visual_required;
	prepared.gpu_resident_skip_cpu_meshing =
		prepared.gpu_resident_visual_only && !collision_required;
	prepared.dependencies.reserve(record->dependencies.size());
	for (const Dependency &dependency : record->dependencies) {
		if (!dependency.page) source_valid = false;
		prepared.dependencies.push_back({ dependency.key, dependency.page });
	}
	if (!source_valid) {
		record->phase = WtPageMeshingRuntimePhase::MeshFailedReady;
		++metrics_.mesh_failures;
		record_execution_failure_key(metrics_, record->key);
		submit_pending_result(record_index, scheduler);
		record_time();
		return WtPageMeshingRuntimeStatus::MeshingFailure;
	}
	record->phase = WtPageMeshingRuntimePhase::Meshing;
	record_time();
	return WtPageMeshingRuntimeStatus::Ok;
}

bool WtPageMeshingRuntimeService::find_edited_page(
	const WtChunkKey &key,
	std::uint64_t source_revision,
	std::uint64_t maximum_world_revision,
	std::uint64_t &world_revision,
	std::shared_ptr<const WtChunkPage> &page
) noexcept {
	EditedPageEntry *best = nullptr;
	for (EditedPageEntry &entry : edited_pages_) {
		if (entry.key != key || entry.source_revision != source_revision ||
			entry.world_revision > maximum_world_revision || !entry.page) {
			continue;
		}
		if (best == nullptr || entry.world_revision > best->world_revision) {
			best = &entry;
		}
	}
	if (best == nullptr) return false;
	best->last_touch = ++edited_page_touch_;
	world_revision = best->world_revision;
	page = best->page;
	return true;
}

void WtPageMeshingRuntimeService::store_edited_page(
	const WtChunkKey &key,
	std::uint64_t source_revision,
	std::uint64_t world_revision,
	std::shared_ptr<const WtChunkPage> page
) {
	if (edited_page_capacity_ == 0 || edited_page_byte_capacity_ == 0 || !page) return;
	const std::size_t resident_bytes = sizeof(WtChunkPage) +
		page->samples.capacity() * sizeof(WtScalarSample) +
		page->surface_shift_records.capacity() *
			sizeof(WtChunkSurfaceShiftRecord);
	if (resident_bytes > edited_page_byte_capacity_) return;
	for (auto entry = edited_pages_.begin(); entry != edited_pages_.end(); ++entry) {
		if (entry->key != key || entry->source_revision != source_revision) continue;
		metrics_.edited_page_cache_resident_bytes -= entry->resident_bytes;
		edited_pages_.erase(entry);
		break;
	}
	while (!edited_pages_.empty() &&
		(edited_pages_.size() >= edited_page_capacity_ ||
			metrics_.edited_page_cache_resident_bytes >
				edited_page_byte_capacity_ - resident_bytes)) {
		const auto oldest = std::min_element(
			edited_pages_.begin(), edited_pages_.end(),
			[](const EditedPageEntry &left, const EditedPageEntry &right) {
				return left.last_touch < right.last_touch;
			}
		);
		metrics_.edited_page_cache_resident_bytes -= oldest->resident_bytes;
		edited_pages_.erase(oldest);
		++metrics_.edited_page_cache_evictions;
	}
	edited_pages_.push_back({
		key, source_revision, world_revision, ++edited_page_touch_,
		resident_bytes, std::move(page),
	});
	metrics_.edited_page_cache_resident_bytes += resident_bytes;
	metrics_.edited_page_cache_entries = edited_pages_.size();
	++metrics_.edited_page_cache_updates;
}

WtPageMeshingRuntimeService::PreparedMeshCompletion
WtPageMeshingRuntimeService::execute_prepared_mesh_job(
	PreparedMeshJob prepared,
	const WtChunkMesher &mesher,
	WtChunkMeshingScratch &scratch
) {
	PreparedMeshCompletion completion;
	completion.prepared = std::move(prepared);
	completion.gpu_resident_visual_only =
		completion.prepared.gpu_resident_visual_only;
	completion.gpu_resident_skip_cpu_meshing =
		completion.prepared.gpu_resident_skip_cpu_meshing;
	completion.collision_dirty_regular_brick_mask =
		completion.prepared.dirty_regular_brick_mask;
	const auto primary = std::lower_bound(
		completion.prepared.dependencies.begin(),
		completion.prepared.dependencies.end(),
		completion.prepared.job.key,
		[](const PreparedDependency &left, const WtChunkKey &right) {
			return left.key < right;
		}
	);
	bool source_valid = primary != completion.prepared.dependencies.end() &&
		primary->key == completion.prepared.job.key &&
		static_cast<bool>(primary->page);
	std::unique_ptr<WtChunkPageSampleSource> source;
	if (source_valid) {
		source = std::make_unique<WtChunkPageSampleSource>(*primary->page);
		for (const PreparedDependency &dependency :
			completion.prepared.dependencies) {
			if (dependency.key == completion.prepared.job.key) continue;
			if (!dependency.page ||
				source->add_transition_support_page(*dependency.page) !=
					WtChunkPageSampleSourceStatus::Ok) {
				source_valid = false;
				break;
			}
		}
		source_valid = source_valid && source->has_transition_support(
			completion.prepared.cached_transition_mask
		);
	}
	completion.mesh = std::make_shared<WtChunkMeshResult>();
	bool water_present = false;
	if (source_valid && (completion.prepared.visual_required ||
		completion.prepared.pre_mesh_field_capture)) {
		bool explicit_water_inside = false;
		bool explicit_water_outside = false;
		for (const PreparedDependency &dependency :
			completion.prepared.dependencies) {
			if (!dependency.page) continue;
			if (dependency.page->static_water_summary_valid) {
				explicit_water_inside = explicit_water_inside ||
					dependency.page->static_water_explicit_inside;
				explicit_water_outside = explicit_water_outside ||
					dependency.page->static_water_explicit_outside;
				water_present = water_present ||
					dependency.page->static_water_occupied;
				water_present = water_present ||
					(explicit_water_inside && explicit_water_outside);
				if (water_present) break;
				continue;
			}
			for (const WtScalarSample &sample : dependency.page->samples) {
				if (sample.static_water_density != kWtNoStaticWaterDensity) {
					explicit_water_inside = explicit_water_inside ||
						sample.static_water_density < 0.0F;
					explicit_water_outside = explicit_water_outside ||
						sample.static_water_density >= 0.0F;
					water_present = water_present ||
						(sample.static_water_density < 0.0F &&
							WtMaterialVolumeSampleSource::is_occupied(
								sample, kWtStaticWaterMaterialId
							));
				} else if (WtMaterialVolumeSampleSource::is_occupied(
						sample, kWtStaticWaterMaterialId
					)) {
					water_present = true;
					break;
				}
			}
			water_present = water_present ||
				(explicit_water_inside && explicit_water_outside);
			if (water_present) break;
		}
	}
	const auto capture_pre_mesh_field = [
		&completion, water_present
	](WtGpuMeshingShadowSurface surface) {
		WtGpuMeshingShadowCapture capture;
		capture.job = completion.prepared.job;
		capture.transition_mask = completion.prepared.transition_mask;
		capture.cached_transition_mask =
			completion.prepared.cached_transition_mask;
		capture.surface = surface;
		capture.capture_stage = WtGpuMeshingCaptureStage::PreMeshField;
		capture.static_water_surface_expected = water_present;
		capture.cpu_visual_mesh_omitted =
			completion.prepared.gpu_resident_visual_only;
		capture.incremental_edit = completion.prepared.incremental_edit;
		capture.dirty_regular_brick_mask =
			completion.prepared.dirty_regular_brick_mask;
		capture.dirty_edit_bounds = completion.prepared.dirty_edit_bounds;
		capture.has_dirty_edit_bounds = completion.prepared.has_dirty_edit_bounds;
		capture.retained_pages.reserve(completion.prepared.dependencies.size());
		for (const PreparedDependency &dependency :
				completion.prepared.dependencies) {
			capture.retained_pages.push_back({ dependency.key, dependency.page });
		}
		if (capture.retained_pages.empty()) return false;
		if (completion.prepared.cell_capture_callback) {
			completion.prepared.cell_capture_callback(std::move(capture));
		} else if (completion.prepared.defer_gpu_capture) {
			completion.deferred_gpu_captures.push_back(std::move(capture));
		} else {
			return false;
		}
		return true;
	};
	const auto initialize_gpu_placeholder_mesh = [&completion](
		WtChunkMeshResult &mesh
	) {
		mesh.key = completion.prepared.job.key;
		mesh.world_origin = wt_chunk_bounds(completion.prepared.job.key).minimum;
		mesh.transition_mask = completion.prepared.transition_mask;
		mesh.cached_transition_mask =
			completion.prepared.cached_transition_mask;
	};
	const auto build_gpu_collision_mesh = [&]() {
		if (!completion.prepared.collision_required &&
			!completion.prepared.gpu_lod0_collision_prewarm) {
			return WtChunkMeshingStatus::Ok;
		}
		if (completion.prepared.job.key.lod != 0) {
			return WtChunkMeshingStatus::InvalidInput;
		}
		completion.collision_patch_mesh =
			std::make_shared<WtChunkMeshResult>();
		if (completion.prepared.incremental_edit &&
				completion.prepared.live_collision_patch_base) {
			completion.collision_dirty_regular_brick_mask =
				completion.prepared.dirty_regular_brick_mask;
		} else {
			// Cold collision and an edit without a verified live block set need a
			// complete authoritative base. Regular collision blocks contain exactly
			// the physics geometry; transition and render payloads stay on the GPU.
			completion.collision_dirty_regular_brick_mask = 0xff;
		}
		return mesher.mesh_regular_collision_blocks(
			{
				completion.prepared.job.key,
				0,
				0,
				0.0F,
				0.25F,
			},
			*source,
			completion.collision_dirty_regular_brick_mask,
			*completion.collision_patch_mesh,
			scratch
		);
	};
	WtChunkMeshingStatus terrain_status =
		WtChunkMeshingStatus::SampleSourceFailure;
	if (source_valid && completion.prepared.pre_mesh_field_capture &&
		(completion.prepared.cell_capture_callback ||
			completion.prepared.defer_gpu_capture)) {
		if (!capture_pre_mesh_field(WtGpuMeshingShadowSurface::Terrain)) {
			terrain_status = WtChunkMeshingStatus::CellBackendFailure;
		} else if (completion.prepared.gpu_resident_skip_cpu_meshing ||
				(completion.prepared.collision_required &&
					completion.prepared.job.key.lod == 0) ||
				completion.prepared.gpu_lod0_collision_prewarm) {
			terrain_status = build_gpu_collision_mesh();
			initialize_gpu_placeholder_mesh(*completion.mesh);
		} else {
			terrain_status = mesher.mesh(
				{
					completion.prepared.job.key,
					completion.prepared.transition_mask,
					completion.prepared.cached_transition_mask,
					0.0F,
					0.25F,
				},
				*source,
				*completion.mesh,
				scratch
			);
		}
	} else if (source_valid && completion.prepared.cell_capture_callback &&
		!completion.prepared.pre_mesh_field_capture) {
		WtRecordingMeshingBackend recording(mesher.backend());
		terrain_status = WtChunkMesher(recording).mesh(
			{
				completion.prepared.job.key,
				completion.prepared.transition_mask,
				completion.prepared.cached_transition_mask,
				0.0F,
				0.25F,
			},
			*source,
			*completion.mesh,
			scratch
		);
		if (!recording.overflowed()) {
			completion.terrain_records = recording.take_records();
		} else {
			terrain_status = WtChunkMeshingStatus::CellBackendFailure;
		}
	} else if (source_valid) {
		terrain_status = mesher.mesh(
			{
				completion.prepared.job.key,
				completion.prepared.transition_mask,
				completion.prepared.cached_transition_mask,
				0.0F,
				0.25F,
			},
			*source,
			*completion.mesh,
			scratch
		);
	}
	const bool mesh_ok = terrain_status == WtChunkMeshingStatus::Ok;
	if (mesh_ok && completion.collision_patch_mesh &&
		completion.prepared.early_collision_ready) {
		completion.collision_emitted_early =
			completion.prepared.early_collision_ready({
				completion.prepared.job.key,
				completion.prepared.job.generation,
				completion.collision_patch_mesh,
				completion.prepared.incremental_edit,
				completion.collision_dirty_regular_brick_mask,
				true,
			});
	}
	completion.water_mesh = std::make_shared<WtChunkMeshResult>();
	bool water_mesh_ok = mesh_ok;
	if (mesh_ok && (completion.prepared.visual_required ||
		completion.prepared.pre_mesh_field_capture) && water_present) {
		WtChunkMeshingStatus water_status =
			WtChunkMeshingStatus::CellBackendFailure;
		if (completion.prepared.pre_mesh_field_capture &&
			(completion.prepared.cell_capture_callback ||
				completion.prepared.defer_gpu_capture)) {
			if (!capture_pre_mesh_field(
					WtGpuMeshingShadowSurface::StaticWater)) {
				water_status = WtChunkMeshingStatus::CellBackendFailure;
			} else if (completion.prepared.gpu_resident_visual_only) {
				initialize_gpu_placeholder_mesh(*completion.water_mesh);
				water_status = WtChunkMeshingStatus::Ok;
			} else {
				const WtMaterialVolumeSampleSource water_source(
					*source,
					kWtStaticWaterMaterialId
				);
				water_status = mesher.mesh(
					{
						completion.prepared.job.key,
						completion.prepared.transition_mask,
						completion.prepared.cached_transition_mask,
						0.0F,
						0.25F,
					},
					water_source,
					*completion.water_mesh,
					scratch
				);
			}
		} else if (completion.prepared.cell_capture_callback) {
			const WtMaterialVolumeSampleSource water_source(
				*source,
				kWtStaticWaterMaterialId
			);
			WtRecordingMeshingBackend recording(mesher.backend());
			water_status = WtChunkMesher(recording).mesh(
				{
					completion.prepared.job.key,
					completion.prepared.transition_mask,
					completion.prepared.cached_transition_mask,
					0.0F,
					0.25F,
				},
				water_source,
				*completion.water_mesh,
				scratch
			);
			if (!recording.overflowed()) {
				completion.water_records = recording.take_records();
			} else {
				water_status = WtChunkMeshingStatus::CellBackendFailure;
			}
		} else {
			const WtMaterialVolumeSampleSource water_source(
				*source,
				kWtStaticWaterMaterialId
			);
			water_status = mesher.mesh(
				{
					completion.prepared.job.key,
					completion.prepared.transition_mask,
					completion.prepared.cached_transition_mask,
					0.0F,
					0.25F,
				},
				water_source,
				*completion.water_mesh,
				scratch
			);
		}
		water_mesh_ok = water_status == WtChunkMeshingStatus::Ok;
	} else if (mesh_ok) {
		initialize_gpu_placeholder_mesh(*completion.water_mesh);
	}
	completion.status = mesh_ok && water_mesh_ok ?
		WtPageMeshingRuntimeStatus::Ok :
		WtPageMeshingRuntimeStatus::MeshingFailure;
	return completion;
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::accept_prepared_mesh_completion(
	PreparedMeshCompletion completion,
	WtStreamScheduler &scheduler,
	bool synchronous_compatibility
) {
	const std::uint64_t started = steady_time_ns();
	const auto record_time = [this, started]() {
		const std::uint64_t elapsed = steady_time_ns() - started;
		metrics_.mesh_completion_time_ns_last = elapsed;
		metrics_.mesh_completion_time_ns_total += elapsed;
		metrics_.mesh_completion_time_ns_maximum = std::max(
			metrics_.mesh_completion_time_ns_maximum,
			elapsed
		);
	};
	auto record = find_record(completion.prepared.job.key);
	const WtChunkRecord *scheduler_record =
		scheduler.find_record(completion.prepared.job.key);
	const WtPageMeshingRuntimePhase expected_phase = synchronous_compatibility ?
		WtPageMeshingRuntimePhase::AwaitingMesh :
		WtPageMeshingRuntimePhase::Meshing;
	if (record == records_.end() || scheduler_record == nullptr ||
		record->generation != completion.prepared.job.generation ||
		record->source_revision != completion.prepared.job.source_revision ||
		record->world_revision != completion.prepared.job.world_revision ||
		record->phase != expected_phase ||
		scheduler_record->generation != completion.prepared.job.generation ||
		scheduler_record->source_revision !=
			completion.prepared.job.source_revision ||
		scheduler_record->world_revision !=
			completion.prepared.job.world_revision ||
		scheduler_record->lifecycle != WtChunkLifecycle::Meshing) {
		++metrics_.discarded_mesh_completions;
		record_time();
		return WtPageMeshingRuntimeStatus::StaleCompletion;
	}
	const std::size_t record_index = static_cast<std::size_t>(
		record - records_.begin()
	);
	bool collision_completed_early = completion.collision_emitted_early;
	if (completion.status == WtPageMeshingRuntimeStatus::Ok &&
		(!completion.gpu_resident_skip_cpu_meshing ||
			completion.collision_patch_mesh) &&
		completion.prepared.terrain_mesh_ready && !collision_completed_early) {
		const std::shared_ptr<WtChunkMeshResult> &collision_mesh =
			completion.collision_patch_mesh ?
				completion.collision_patch_mesh : completion.mesh;
		if (!completion.prepared.terrain_mesh_ready({
				record->key,
				record->generation,
				collision_mesh,
				completion.prepared.incremental_edit,
				completion.collision_dirty_regular_brick_mask,
				completion.collision_patch_mesh != nullptr,
			})) {
			record_time();
			return WtPageMeshingRuntimeStatus::TerrainMeshReadyCallbackFailure;
		}
		collision_completed_early = true;
	}
	if (completion.status == WtPageMeshingRuntimeStatus::Ok &&
		completion.prepared.cell_capture_callback &&
		!completion.prepared.pre_mesh_field_capture) {
		auto make_capture = [&](WtGpuMeshingShadowSurface surface) {
			WtGpuMeshingShadowCapture capture;
			capture.job = completion.prepared.job;
			capture.transition_mask = completion.prepared.transition_mask;
			capture.cached_transition_mask =
				completion.prepared.cached_transition_mask;
			capture.surface = surface;
			capture.static_water_surface_expected =
				!completion.water_records.empty();
			capture.authority_terrain_mesh = completion.mesh;
			capture.authority_water_mesh = completion.water_mesh;
			capture.retained_pages.reserve(
				completion.prepared.dependencies.size()
			);
			for (const PreparedDependency &dependency :
					completion.prepared.dependencies) {
				capture.retained_pages.push_back({
					dependency.key, dependency.page
				});
			}
			return capture;
		};
		if (!completion.terrain_records.empty()) {
			WtGpuMeshingShadowCapture capture = make_capture(
				WtGpuMeshingShadowSurface::Terrain
			);
			capture.records = std::move(completion.terrain_records);
			completion.prepared.cell_capture_callback(std::move(capture));
		}
		if (!completion.water_records.empty()) {
			WtGpuMeshingShadowCapture capture = make_capture(
				WtGpuMeshingShadowSurface::StaticWater
			);
			capture.records = std::move(completion.water_records);
			completion.prepared.cell_capture_callback(std::move(capture));
		}
	}
	for (Dependency &dependency : record->dependencies) {
		dependency.page.reset();
	}
	if (completion.status != WtPageMeshingRuntimeStatus::Ok ||
		!completion.mesh || !completion.water_mesh) {
		record->phase = WtPageMeshingRuntimePhase::MeshFailedReady;
		++metrics_.mesh_failures;
		record_execution_failure_key(metrics_, record->key);
		const WtPageMeshingRuntimeStatus submit_status =
			submit_pending_result(record_index, scheduler);
		record_time();
		return submit_status == WtPageMeshingRuntimeStatus::Ok ?
			WtPageMeshingRuntimeStatus::MeshingFailure : submit_status;
	}
	record->mesh = std::move(completion.mesh);
	record->water_mesh = std::move(completion.water_mesh);
	record->gpu_resident_visual_only = completion.gpu_resident_visual_only;
	record->collision_completed_early = collision_completed_early;
	if (collision_completed_early && completion.prepared.pre_mesh_field_capture &&
			(completion.prepared.collision_required ||
				completion.prepared.gpu_lod0_collision_prewarm)) {
		++metrics_.gpu_collision_block_completions;
		if (completion.prepared.incremental_edit &&
				completion.prepared.live_collision_patch_base) {
			++metrics_.gpu_collision_incremental_block_completions;
		} else {
			++metrics_.gpu_collision_full_block_completions;
		}
		if (!completion.prepared.collision_required &&
			completion.prepared.gpu_lod0_collision_prewarm) {
			++metrics_.gpu_collision_prewarm_completions;
		}
	}
	record->incremental_edit = completion.prepared.incremental_edit;
	record->dirty_regular_brick_mask =
		completion.prepared.dirty_regular_brick_mask;
	if (!completion.deferred_gpu_captures.empty()) {
		record->deferred_gpu_captures =
			std::move(completion.deferred_gpu_captures);
		if (!completion.prepared.visual_required) {
			record->phase = WtPageMeshingRuntimePhase::MeshReady;
			++metrics_.mesh_successes;
			const WtPageMeshingRuntimeStatus status =
				submit_pending_result(record_index, scheduler);
			record_time();
			return status;
		}
		record->phase = WtPageMeshingRuntimePhase::AwaitingGpuCapture;
		++metrics_.mesh_successes;
		record_time();
		return WtPageMeshingRuntimeStatus::Ok;
	}
	record->phase = WtPageMeshingRuntimePhase::MeshReady;
	++metrics_.mesh_successes;
	if (completion.gpu_resident_visual_only) {
		++metrics_.gpu_resident_visual_only_completions;
	}
	const WtPageMeshingRuntimeStatus status =
		submit_pending_result(record_index, scheduler);
	record_time();
	return status;
}

bool WtPageMeshingRuntimeService::peek_deferred_gpu_capture(
	WtChunkJob &job,
	const std::function<bool(const WtChunkJob &)> &eligible
) const noexcept {
	const Record *selected = nullptr;
	for (const Record &record : records_) {
		if ((record.phase != WtPageMeshingRuntimePhase::AwaitingGpuCapture &&
				record.phase != WtPageMeshingRuntimePhase::Ready) ||
			record.deferred_gpu_captures.empty()) {
			continue;
		}
		if (eligible && !eligible(record.deferred_gpu_captures.front().job)) {
			continue;
		}
		if (selected == nullptr || record.priority > selected->priority ||
			(record.priority == selected->priority &&
				record.deferred_gpu_captures.front().job.sequence <
					selected->deferred_gpu_captures.front().job.sequence)) {
			selected = &record;
		}
	}
	if (selected == nullptr) return false;
	job = selected->deferred_gpu_captures.front().job;
	return true;
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::submit_deferred_gpu_capture(
	const WtChunkJob &job,
	const WtMeshCellCaptureCallback &cell_capture_callback,
	WtStreamScheduler &scheduler
) {
	if (!cell_capture_callback) {
		return WtPageMeshingRuntimeStatus::InvalidConfiguration;
	}
	auto record = find_record(job.key);
	const WtChunkRecord *scheduler_record = scheduler.find_record(job.key);
	if (record == records_.end() || scheduler_record == nullptr ||
		(record->phase != WtPageMeshingRuntimePhase::AwaitingGpuCapture &&
			record->phase != WtPageMeshingRuntimePhase::Ready) ||
		record->generation != job.generation ||
		record->source_revision != job.source_revision ||
		record->world_revision != job.world_revision ||
		scheduler_record->generation != job.generation ||
		(scheduler_record->lifecycle != WtChunkLifecycle::Meshing &&
			scheduler_record->lifecycle != WtChunkLifecycle::Ready)) {
		return WtPageMeshingRuntimeStatus::StaleCompletion;
	}
	for (WtGpuMeshingShadowCapture &capture :
			record->deferred_gpu_captures) {
		cell_capture_callback(std::move(capture));
	}
	record->deferred_gpu_captures.clear();
	if (record->phase == WtPageMeshingRuntimePhase::Ready) {
		return WtPageMeshingRuntimeStatus::Ok;
	}
	record->phase = WtPageMeshingRuntimePhase::MeshReady;
	const std::size_t record_index = static_cast<std::size_t>(
		record - records_.begin()
	);
	return submit_pending_result(record_index, scheduler);
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::discard_deferred_gpu_capture(
	const WtChunkJob &job,
	WtStreamScheduler &scheduler
) {
	auto record = find_record(job.key);
	const WtChunkRecord *scheduler_record = scheduler.find_record(job.key);
	if (record == records_.end() || scheduler_record == nullptr ||
		record->phase != WtPageMeshingRuntimePhase::AwaitingGpuCapture ||
		record->generation != job.generation ||
		scheduler_record->generation != job.generation ||
		scheduler_record->lifecycle != WtChunkLifecycle::Meshing) {
		return WtPageMeshingRuntimeStatus::StaleCompletion;
	}
	record->deferred_gpu_captures.clear();
	record->gpu_resident_visual_only = false;
	record->phase = WtPageMeshingRuntimePhase::MeshReady;
	const std::size_t record_index = static_cast<std::size_t>(
		record - records_.begin()
	);
	return submit_pending_result(record_index, scheduler);
}

std::size_t WtPageMeshingRuntimeService::deferred_gpu_capture_count()
		const noexcept {
	return static_cast<std::size_t>(std::count_if(
		records_.begin(), records_.end(), [](const Record &record) {
			return !record.deferred_gpu_captures.empty();
		}
	));
}

} // namespace world_transvoxel
