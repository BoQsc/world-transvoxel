#include "services/wt_read_only_world_runtime.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "meshing/wt_chunk_mesher.h"
#include "physics/wt_collision_builder.h"
#include "render/wt_render_payload.h"
#include "services/wt_chunk_application.h"
#include "services/wt_chunk_resource_cache.h"
#include "services/wt_desired_set_runtime.h"
#include "services/wt_edit_runtime_replacement.h"
#include "services/wt_page_meshing_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_edit_journal_store.h"
#include "storage/wt_storage_page_cache.h"
#include "editing/wt_edit_spatial_index.h"
#include "streaming/wt_stream_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace world_transvoxel {
namespace {

class GpuMeshingCaptureReservation {
public:
	GpuMeshingCaptureReservation(
		std::shared_ptr<WtGpuMeshingShadowQueue> queue,
		std::uint64_t reservation_id
	) noexcept :
		queue_(std::move(queue)),
		reservation_id_(reservation_id) {}

	~GpuMeshingCaptureReservation() {
		if (queue_) queue_->release_capture_slots(reservation_id_);
	}

	void capture(WtGpuMeshingShadowCapture capture) const {
		if (queue_) {
			queue_->capture_reserved(reservation_id_, std::move(capture));
		}
	}

private:
	std::shared_ptr<WtGpuMeshingShadowQueue> queue_;
	std::uint64_t reservation_id_ = 0;
};

const WtLodMapEntry *find_plan_entry(
	const std::vector<WtLodMapEntry> &entries,
	const WtChunkKey &key
) noexcept {
	const auto iterator = std::lower_bound(
		entries.begin(), entries.end(), key,
		[](const WtLodMapEntry &entry, const WtChunkKey &value) {
			return entry.key < value;
		}
	);
	return iterator != entries.end() && iterator->key == key ? &*iterator :
		nullptr;
}

} // namespace

void WtReadOnlyWorldRuntime::queue_transition_remeshes(
	const std::vector<WtDesiredChunk> &chunks
) {
	for (const WtDesiredChunk &chunk : chunks) {
		const auto position = std::lower_bound(
			pending_transition_remeshes_.begin(),
			pending_transition_remeshes_.end(),
			chunk.key,
			[](const WtDesiredChunk &item, const WtChunkKey &key) {
				return item.key < key;
			}
		);
		if (position != pending_transition_remeshes_.end() &&
			position->key == chunk.key) {
			*position = chunk;
		} else {
			pending_transition_remeshes_.insert(position, chunk);
		}
	}
}

void WtReadOnlyWorldRuntime::queue_readiness_repair_candidate(
	const WtChunkKey &key
) {
	const auto position = std::lower_bound(
		readiness_repair_candidate_keys_.begin(),
		readiness_repair_candidate_keys_.end(),
		key
	);
	if (position == readiness_repair_candidate_keys_.end() ||
		*position != key) {
		readiness_repair_candidate_keys_.insert(position, key);
	}
}

bool WtReadOnlyWorldRuntime::publish_delta(
	const WtDesiredSetDelta &delta
) {
	// Publish additions before removals. The Godot/front-end application keeps
	// old visible chunks alive while replacements are staged, but it can only do
	// that correctly for chunks it already knows are expected. If removals are
	// published first during a large viewer movement, the front-end can retire
	// old chunks before it has received all new chunk expectations, producing
	// visible rectangular skybox holes while the scheduler is still working.
	bool contains_replacement = !delta.removed.empty();
	for (const WtDesiredChunk &item : delta.updated) {
		const WtDesiredChunk *previous = desired_->find_desired(item.key);
		contains_replacement = contains_replacement ||
			(previous != nullptr && previous->visual_required &&
				!item.visual_required);
	}
	for (const WtDesiredChunk &item : delta.added) {
		const WtChunkRecord *record = scheduler_->find_record(item.key);
		if (record == nullptr) return false;
		const bool addition_staged_replacement = contains_replacement;
		WtReadOnlyPublication publication;
		publication.kind = WtReadOnlyPublicationKind::ExpectChunk;
		publication.key = item.key;
		publication.generation = record->generation;
		publication.collision_required = item.collision_required;
		publication.visual_required = item.visual_required;
		publication.staged_replacement = addition_staged_replacement;
		if (!push_publication(std::move(publication))) return false;
		if (addition_staged_replacement) {
			const WtApplicationStatus application_status =
				application_->expect_chunk(
					item.key,
					record->generation,
					item.collision_required,
					item.visual_required,
					true,
					item.collision_required
				);
			if (application_status != WtApplicationStatus::Ok &&
				application_status != WtApplicationStatus::AlreadyCurrent) {
				return false;
			}
			queue_readiness_repair_candidate(item.key);
		}
		const auto render = item.visual_required ?
			resource_cache_->find_render(item.key, record->generation) :
			std::shared_ptr<const WtRenderPayload>{};
		if (render) {
			WtReadOnlyPublication render_publication;
			render_publication.kind = WtReadOnlyPublicationKind::RenderPayload;
			render_publication.key = render->key;
			render_publication.generation = render->generation;
			render_publication.render = render;
			render_publication.staged_replacement =
				addition_staged_replacement;
			if (!push_publication(std::move(render_publication))) return false;
		}
		if (item.collision_required) {
			const auto collision = resource_cache_->find_collision(
				item.key,
				record->generation
			);
			if (collision) {
				WtReadOnlyPublication collision_publication;
				collision_publication.kind =
					WtReadOnlyPublicationKind::CollisionPayload;
				collision_publication.key = collision->key;
				collision_publication.generation = collision->generation;
				collision_publication.collision_required = true;
				collision_publication.collision = collision;
				if (!push_publication(std::move(collision_publication))) return false;
			}
		}
	}
	for (const WtDesiredChunk &item : delta.updated) {
		const WtDesiredChunk *previous = desired_->find_desired(item.key);
		if (previous == nullptr) return false;
		// Numeric job priority changes on almost every viewer movement and is
		// consumed by the worker-side scheduler. Publishing those changes as
		// collision state floods the bounded front-end queue with obsolete
		// SetCollisionRequired messages, delaying the first real activation.
		const WtChunkRecord *record = scheduler_->find_record(item.key);
		if (record == nullptr) return false;
		if (previous->collision_required != item.collision_required &&
			item.collision_required && !push_publication({
				WtReadOnlyPublicationKind::SetCollisionRequired,
				item.key,
				record->generation,
				true,
				{},
				{},
			})) return false;
		if (previous->visual_required != item.visual_required) {
			if (item.visual_required) {
				WtReadOnlyPublication expectation;
				expectation.kind = WtReadOnlyPublicationKind::ExpectChunk;
				expectation.key = item.key;
				expectation.generation = record->generation;
				expectation.collision_required = item.collision_required;
				expectation.visual_required = true;
				expectation.staged_replacement = true;
				const WtApplicationStatus application_status =
					application_->expect_chunk(
						item.key,
						record->generation,
						item.collision_required,
						true,
						true,
						item.collision_required
					);
				if (application_status != WtApplicationStatus::Ok &&
					application_status != WtApplicationStatus::AlreadyCurrent) {
					return false;
				}
				if (!push_publication(std::move(expectation))) return false;
				queue_readiness_repair_candidate(item.key);
			}
			WtReadOnlyPublication visual;
			visual.kind = WtReadOnlyPublicationKind::SetVisualRequired;
			visual.key = item.key;
			visual.generation = record->generation;
			visual.visual_required = item.visual_required;
			visual.staged_replacement = !item.visual_required;
			if (!push_publication(std::move(visual))) return false;
		}
		if (previous->collision_required != item.collision_required &&
			!item.collision_required && !push_publication({
				WtReadOnlyPublicationKind::SetCollisionRequired,
				item.key,
				record->generation,
				false,
				{},
				{},
			})) return false;
	}
	for (const WtChunkKey &key : delta.removed) {
		if (!push_publication({
				WtReadOnlyPublicationKind::RemoveChunk,
				key,
				{},
				false,
				{},
				{},
			})) return false;
	}
	return true;
}

bool WtReadOnlyWorldRuntime::publish_transition_mask_update(
	const WtLodMapEntry &entry,
	const WtDesiredChunk &desired
) {
	if ((entry.transition_mask & 0xC0U) != 0 ||
		(entry.key.lod == 0 && entry.transition_mask != 0)) {
		return false;
	}
	const WtChunkRecord *record = scheduler_->find_record(entry.key);
	if (record == nullptr || record->lifecycle != WtChunkLifecycle::Ready) {
		return true;
	}
	WtChunkApplicationRecord application_record;
	if (!application_->copy_record(entry.key, application_record) ||
		application_record.generation != record->generation ||
		!application_record.visual_required ||
		!desired.visual_required) {
		return true;
	}
	const auto mesh = resource_cache_->find_mesh(entry.key, record->generation);
	if (!mesh) {
		queue_transition_remeshes({ desired });
		return true;
	}
	if (mesh->transition_mask != entry.transition_mask) {
		queue_transition_remeshes({ desired });
		return true;
	}
	const auto water_mesh = resource_cache_->find_water_mesh(
		entry.key,
		record->generation
	);
	auto render = std::make_shared<WtRenderPayload>();
	const WtRenderBuildStatus render_status = water_mesh ?
		wt_build_render_payload(
			*mesh,
			*water_mesh,
			record->generation,
			entry.transition_mask,
			*render
		) :
		wt_build_render_payload(
			*mesh,
			record->generation,
			entry.transition_mask,
			*render
		);
	if (render_status != WtRenderBuildStatus::Ok) {
		queue_transition_remeshes({ desired });
		return true;
	}
	if (!water_mesh) {
		const auto previous = resource_cache_->find_render(
			entry.key,
			record->generation
		);
		if (previous) {
			render->water_vertices = previous->water_vertices;
			render->water_indices = previous->water_indices;
		}
	}
	if (resource_cache_->insert_render(render, record->generation) !=
		WtChunkResourceCacheStatus::Ok) {
		queue_transition_remeshes({ desired });
		return true;
	}
	WtReadOnlyPublication publication;
	publication.kind = WtReadOnlyPublicationKind::RenderPayload;
	publication.key = render->key;
	publication.generation = render->generation;
	publication.render = std::move(render);
	publication.staged_replacement = application_record.staged_replacement;
	return push_publication(std::move(publication));
}

bool WtReadOnlyWorldRuntime::process_pending_transition_remeshes() {
	bool progressed = false;
	for (std::size_t index = 0; index < pending_transition_remeshes_.size();) {
		if (scheduler_->available_job_capacity() == 0) {
			break;
		}
		const WtDesiredChunk item = pending_transition_remeshes_[index];
		const WtDesiredChunk *desired = desired_->find_desired(item.key);
		if (desired == nullptr ||
			find_plan_entry(current_plan_.entries, item.key) == nullptr) {
			pending_transition_remeshes_.erase(
				pending_transition_remeshes_.begin() + index
			);
			progressed = true;
			continue;
		}
		const WtChunkRecord *record = scheduler_->find_record(item.key);
		if (record == nullptr) {
			pending_transition_remeshes_.erase(
				pending_transition_remeshes_.begin() + index
			);
			progressed = true;
			continue;
		}
		if (record->lifecycle != WtChunkLifecycle::Ready) {
			++index;
			continue;
		}
		const WtSchedulerStatus scheduler_status =
			scheduler_->request_chunk_version(
				item.key,
				storage_.source_revision(),
				world_revision_.load(),
				desired->priority,
				true
			);
		if (scheduler_status == WtSchedulerStatus::JobQueueFull) {
			break;
		}
		if (scheduler_status != WtSchedulerStatus::Ok) {
			set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
			return true;
		}
		record = scheduler_->find_record(item.key);
		if (record != nullptr) {
			causal_trace_.record(
				WtCausalTraceEventKind::TransitionRemeshGenerationCreated,
				WtCausalTraceThreadRole::Runtime,
				&item.key,
				record->generation,
				world_revision_.load()
			);
		}
		const WtApplicationStatus application_status =
			record == nullptr ? WtApplicationStatus::NotFound :
			application_->expect_chunk(
				item.key,
				record->generation,
				desired->collision_required,
				desired->visual_required,
				true,
				desired->collision_required
			);
		if (application_status != WtApplicationStatus::Ok &&
			application_status != WtApplicationStatus::AlreadyCurrent) {
			set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
			return true;
		}
		WtReadOnlyPublication expectation;
		expectation.kind = WtReadOnlyPublicationKind::ExpectChunk;
		expectation.key = item.key;
		expectation.generation = record->generation;
		expectation.collision_required = desired->collision_required;
		expectation.visual_required = desired->visual_required;
		expectation.staged_replacement = true;
		expectation.preserve_collision_ready = desired->collision_required;
		if (!push_publication(std::move(expectation))) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			return true;
		}
		pending_transition_remeshes_.erase(
			pending_transition_remeshes_.begin() + index
		);
		progressed = true;
	}
	return progressed;
}

bool WtReadOnlyWorldRuntime::process_storage_completions() {
	bool progressed = false;
	WtPageLoadCompletion completion;
	while (storage_.pop_completion(completion)) {
		progressed = true;
		causal_trace_.record(
			WtCausalTraceEventKind::StorageCompletionConsumed,
			WtCausalTraceThreadRole::Runtime,
			&completion.key,
			completion.generation,
			0,
			0,
			0,
			static_cast<std::int64_t>(completion.status)
		);
		const WtPageMeshingRuntimeStatus status =
			page_runtime_->accept_storage_completion(
				completion,
				*page_cache_,
				*scheduler_
			);
		if (status != WtPageMeshingRuntimeStatus::Ok &&
			status != WtPageMeshingRuntimeStatus::CompletionNotOwned &&
			status != WtPageMeshingRuntimeStatus::StaleCompletion &&
			status != WtPageMeshingRuntimeStatus::SchedulerBackpressure &&
			status != WtPageMeshingRuntimeStatus::CacheFailure) {
			set_failure(
				WtReadOnlyRuntimeStatus::PipelineStorageCompletionFailure
			);
			break;
		}
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.storage_completions;
	}
	return progressed;
}

bool WtReadOnlyWorldRuntime::process_scheduler_jobs() {
	bool progressed = false;
	WtChunkJob job;
	for (std::size_t count = 0; count < 4; ++count) {
		if (has_pending_edit_operation()) {
			break;
		}
		if (page_runtime_->asynchronous_meshing_enabled() &&
			!page_runtime_->asynchronous_mesh_admission_available()) {
			WtChunkJob next_job;
			if (!scheduler_->peek_job(next_job) ||
				next_job.stage == WtChunkJobStage::Mesh) {
				break;
			}
		}
		if (!scheduler_->pop_job(job)) {
			break;
		}
		progressed = true;
		const auto job_started = std::chrono::steady_clock::now();
		const bool trace_enabled = causal_trace_.enabled();
		WtPageMeshingRuntimeRecordSnapshot trace_mesh_record;
		const bool trace_mesh_record_found =
			job.stage == WtChunkJobStage::Mesh &&
			page_runtime_->copy_record(
				job.key,
				job.generation,
				trace_mesh_record
			);
		const std::uint8_t trace_transition_mask =
			trace_mesh_record_found ? trace_mesh_record.transition_mask : 0;
		const bool asynchronous_mesh =
			job.stage == WtChunkJobStage::Mesh &&
			page_runtime_->asynchronous_meshing_enabled();
		if (trace_enabled && !asynchronous_mesh) {
			causal_trace_.record(
				job.stage == WtChunkJobStage::Sample ?
					WtCausalTraceEventKind::SampleStarted :
					WtCausalTraceEventKind::MeshStarted,
				WtCausalTraceThreadRole::Runtime,
				&job.key,
				job.generation,
				job.world_revision,
				job.sequence
			);
			if (job.stage == WtChunkJobStage::Mesh &&
				trace_transition_mask != 0) {
				causal_trace_.record(
					WtCausalTraceEventKind::TransitionMeshStarted,
					WtCausalTraceThreadRole::Runtime,
					&job.key,
					job.generation,
					job.world_revision,
					trace_transition_mask
				);
			}
		}
		const std::uint64_t traced_job_started_ns = trace_enabled ?
			wt_causal_trace_now_ns() : 0;
		WtPageMeshingRuntimeStatus status;
		if (job.stage == WtChunkJobStage::Sample) {
			const WtLodMapEntry *entry = find_plan_entry(
				current_plan_.entries, job.key
			);
			const std::uint8_t transition_mask =
				entry != nullptr ? entry->transition_mask : 0;
			status = page_runtime_->begin_sample_job(
				job,
				transition_mask,
				transition_mask,
				storage_,
				*page_cache_,
				*scheduler_
			);
			if (trace_enabled) {
				WtPageMeshingRuntimeRecordSnapshot ownership_record;
				if (page_runtime_->copy_record(
						job.key,
						job.generation,
						ownership_record
					)) {
					const WtCausalTraceJobDetails details {
						WtCausalTraceJobStage::Sample,
						ownership_record.priority,
						job.sequence,
					};
					causal_trace_.record(
						WtCausalTraceEventKind::
							PageMeshingOwnershipEstablished,
						WtCausalTraceThreadRole::Runtime,
						&job.key,
						job.generation,
						job.world_revision,
						static_cast<std::uint64_t>(ownership_record.phase),
						0,
						static_cast<std::int64_t>(status),
						&details
					);
				}
			}
		} else {
			WtChunkApplicationRecord application_record;
			if (!application_->copy_record(job.key, application_record) ||
				application_record.generation != job.generation) {
				continue;
			}
			WtTerrainMeshReadyCallback terrain_mesh_ready;
			if (!application_record.visual_required ||
				!application_record.staged_replacement) {
				terrain_mesh_ready =
					[this](const WtTerrainMeshCompletion &completion) {
						return process_terrain_mesh_completion(completion);
					};
			}
			WtMeshCellCaptureCallback cell_capture_callback;
			if (gpu_meshing_shadow_ && gpu_meshing_shadow_->enabled()) {
				const std::shared_ptr<WtGpuMeshingShadowQueue> shadow =
					gpu_meshing_shadow_;
				const std::uint64_t reservation_id =
					shadow->reserve_capture_slots();
				if (reservation_id != 0) {
					const auto reservation = std::make_shared<
						GpuMeshingCaptureReservation
					>(shadow, reservation_id);
					cell_capture_callback = [reservation](
						WtGpuMeshingShadowCapture capture
					) {
						reservation->capture(std::move(capture));
					};
				}
			}
			if (asynchronous_mesh) {
				const WtMeshExecutionCallback execution_callback =
					[this](const WtMeshExecutionEvent &event) {
						if (!causal_trace_.enabled()) return;
						causal_trace_.record(
							event.started ?
								WtCausalTraceEventKind::MeshStarted :
								WtCausalTraceEventKind::MeshFinished,
							WtCausalTraceThreadRole::Meshing,
							&event.job.key,
							event.job.generation,
							event.job.world_revision,
							event.job.sequence,
							event.duration_ns,
							static_cast<std::int64_t>(event.status)
						);
						if (event.transition_mask != 0) {
							causal_trace_.record(
								event.started ?
									WtCausalTraceEventKind::
										TransitionMeshStarted :
									WtCausalTraceEventKind::
										TransitionMeshFinished,
								WtCausalTraceThreadRole::Meshing,
								&event.job.key,
								event.job.generation,
								event.job.world_revision,
								event.transition_mask,
								event.duration_ns,
								static_cast<std::int64_t>(event.status)
							);
						}
					};
				status = page_runtime_->dispatch_mesh_job(
					job,
					*scheduler_,
					edit_journal_store_ != nullptr ?
						&edit_journal_store_->journal() : nullptr,
					initial_world_revision_,
					&storage_,
					terrain_mesh_ready,
					application_record.visual_required,
					execution_callback,
					cell_capture_callback
				);
			} else {
				status = page_runtime_->execute_mesh_job(
					job,
					*mesher_,
					*meshing_scratch_,
					*scheduler_,
					edit_journal_store_ != nullptr ?
						&edit_journal_store_->journal() : nullptr,
					initial_world_revision_,
					&storage_,
					terrain_mesh_ready,
					application_record.visual_required,
					cell_capture_callback
				);
			}
		}
		const auto job_finished = std::chrono::steady_clock::now();
		const std::uint64_t job_time_ns =
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					job_finished - job_started
				).count()
			);
		if (trace_enabled && !asynchronous_mesh) {
			causal_trace_.record(
				job.stage == WtChunkJobStage::Sample ?
					WtCausalTraceEventKind::SampleFinished :
					WtCausalTraceEventKind::MeshFinished,
				WtCausalTraceThreadRole::Runtime,
				&job.key,
				job.generation,
				job.world_revision,
				job.sequence,
				wt_causal_trace_now_ns() - traced_job_started_ns,
				static_cast<std::int64_t>(status)
			);
			if (job.stage == WtChunkJobStage::Mesh &&
				trace_transition_mask != 0) {
				causal_trace_.record(
					WtCausalTraceEventKind::TransitionMeshFinished,
					WtCausalTraceThreadRole::Runtime,
					&job.key,
					job.generation,
					job.world_revision,
					trace_transition_mask,
					wt_causal_trace_now_ns() - traced_job_started_ns,
					static_cast<std::int64_t>(status)
				);
			}
		}
		{
			std::lock_guard<std::mutex> lock(metrics_mutex_);
			if (job.stage == WtChunkJobStage::Sample) {
				++metrics_.sample_jobs;
				metrics_.sample_job_time_ns_last = job_time_ns;
				metrics_.sample_job_time_ns_total += job_time_ns;
				metrics_.sample_job_time_ns_maximum = std::max(
					metrics_.sample_job_time_ns_maximum,
					job_time_ns
				);
			} else {
				++metrics_.mesh_jobs;
				if (!asynchronous_mesh) {
					metrics_.mesh_job_time_ns_last = job_time_ns;
					metrics_.mesh_job_time_ns_total += job_time_ns;
					metrics_.mesh_job_time_ns_maximum = std::max(
						metrics_.mesh_job_time_ns_maximum,
						job_time_ns
					);
				}
			}
		}
		if (status ==
				WtPageMeshingRuntimeStatus::TerrainMeshReadyCallbackFailure) {
			break;
		}
		if (status != WtPageMeshingRuntimeStatus::Ok &&
			status != WtPageMeshingRuntimeStatus::SchedulerBackpressure &&
			status != WtPageMeshingRuntimeStatus::StorageRequestFailure &&
			status != WtPageMeshingRuntimeStatus::CacheFailure &&
			status != WtPageMeshingRuntimeStatus::MeshingFailure &&
			status != WtPageMeshingRuntimeStatus::SurfaceShiftFailure &&
			status != WtPageMeshingRuntimeStatus::NotReady) {
			set_failure(WtReadOnlyRuntimeStatus::PipelineSchedulerJobFailure);
			break;
		}
		if (has_pending_edit_operation()) {
			break;
		}
	}
	return progressed;
}

bool WtReadOnlyWorldRuntime::process_async_mesh_completions() {
	if (!page_runtime_->asynchronous_meshing_enabled()) return false;
	std::size_t processed = 0;
	const WtPageMeshingRuntimeStatus status =
		page_runtime_->process_async_mesh_completions(
			*scheduler_,
			static_cast<std::size_t>(config_.active_chunk_capacity),
			processed
		);
	if (status != WtPageMeshingRuntimeStatus::Ok) {
		set_failure(WtReadOnlyRuntimeStatus::PipelineSchedulerJobFailure);
	}
	return processed != 0;
}
} // namespace world_transvoxel
