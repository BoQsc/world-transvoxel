#include "services/wt_page_meshing_runtime_internal.h"

#include "bake/wt_chunk_baker.h"
#include "editing/wt_chunk_edit_state.h"
#include "meshing/wt_material_volume_sample_source.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_chunk_page_sample_source.h"

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
			WtChunkEditState edit_state;
			if (!dependency.page ||
				edit_state.initialize(
					*dependency.page,
					record->source_revision,
					initial_world_revision,
					procedural_descriptor_pointer
				) != WtChunkEditStatus::Ok ||
				edit_journal->replay_until(record->world_revision, edit_state) !=
					WtEditJournalStatus::Ok ||
				edit_state.current_world_revision() != record->world_revision) {
				source_valid = false;
				break;
			}
			WtChunkPage edited_page = edit_state.page();
			if (!edited_page.surface_shift_valid) {
				if (!edited_source || !edited_source->valid() ||
					wt_build_surface_shift_records(
						edited_page,
						*edited_source,
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
	prepared.terrain_mesh_ready = terrain_mesh_ready;
	prepared.execution_callback = execution_callback;
	prepared.cell_capture_callback = cell_capture_callback;
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

WtPageMeshingRuntimeService::PreparedMeshCompletion
WtPageMeshingRuntimeService::execute_prepared_mesh_job(
	PreparedMeshJob prepared,
	const WtChunkMesher &mesher,
	WtChunkMeshingScratch &scratch
) {
	PreparedMeshCompletion completion;
	completion.prepared = std::move(prepared);
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
	WtChunkMeshingStatus terrain_status =
		WtChunkMeshingStatus::SampleSourceFailure;
	if (source_valid && completion.prepared.cell_capture_callback) {
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
	completion.water_mesh = std::make_shared<WtChunkMeshResult>();
	bool water_present = false;
	if (mesh_ok && completion.prepared.visual_required) {
		bool explicit_water_inside = false;
		bool explicit_water_outside = false;
		for (const PreparedDependency &dependency :
			completion.prepared.dependencies) {
			if (!dependency.page) continue;
			for (const WtScalarSample &sample : dependency.page->samples) {
				if (sample.static_water_density != kWtNoStaticWaterDensity) {
					explicit_water_inside = explicit_water_inside ||
						sample.static_water_density < 0.0F;
					explicit_water_outside = explicit_water_outside ||
						sample.static_water_density >= 0.0F;
					water_present = water_present ||
						(sample.static_water_density < 0.0F &&
							WtMaterialVolumeSampleSource::is_occupied(
								sample,
								kWtStaticWaterMaterialId
							));
				} else if (WtMaterialVolumeSampleSource::is_occupied(
						sample,
						kWtStaticWaterMaterialId
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
	bool water_mesh_ok = mesh_ok;
	if (mesh_ok && completion.prepared.visual_required && water_present) {
		const WtMaterialVolumeSampleSource water_source(
			*source,
			kWtStaticWaterMaterialId
		);
		WtChunkMeshingStatus water_status =
			WtChunkMeshingStatus::CellBackendFailure;
		if (completion.prepared.cell_capture_callback) {
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
		completion.water_mesh->key = completion.prepared.job.key;
		completion.water_mesh->world_origin =
			wt_chunk_bounds(completion.prepared.job.key).minimum;
		completion.water_mesh->transition_mask =
			completion.prepared.transition_mask;
		completion.water_mesh->cached_transition_mask =
			completion.prepared.cached_transition_mask;
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
	if (completion.status == WtPageMeshingRuntimeStatus::Ok &&
		completion.prepared.terrain_mesh_ready &&
		!completion.prepared.terrain_mesh_ready({
			record->key,
			record->generation,
			completion.mesh,
		})) {
		record_time();
		return WtPageMeshingRuntimeStatus::TerrainMeshReadyCallbackFailure;
	}
	if (completion.status == WtPageMeshingRuntimeStatus::Ok &&
		completion.prepared.cell_capture_callback) {
		auto make_capture = [&](WtGpuMeshingShadowSurface surface) {
			WtGpuMeshingShadowCapture capture;
			capture.job = completion.prepared.job;
			capture.transition_mask = completion.prepared.transition_mask;
			capture.cached_transition_mask =
				completion.prepared.cached_transition_mask;
			capture.surface = surface;
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
	record->phase = WtPageMeshingRuntimePhase::MeshReady;
	++metrics_.mesh_successes;
	const WtPageMeshingRuntimeStatus status =
		submit_pending_result(record_index, scheduler);
	record_time();
	return status;
}

} // namespace world_transvoxel
