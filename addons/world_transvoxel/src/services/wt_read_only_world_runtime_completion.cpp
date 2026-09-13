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

bool WtReadOnlyWorldRuntime::prepare_terrain_collision_payload(
	const WtTerrainMeshCompletion &completion,
	std::shared_ptr<WtCollisionPayload> &collision
) {
	const WtChunkRecord *record = scheduler_->find_record(completion.key);
	if (record == nullptr || record->generation != completion.generation ||
		!completion.mesh) {
		return true;
	}
	collision = std::make_shared<WtCollisionPayload>();
	const WtCollisionPolicy collision_policy {
		kWtDefaultCollisionThinRatioSquared,
		config_.collision_activation_distance,
		config_.collision_deactivation_distance,
	};
	WtChunkApplicationRecord application_record;
	if (!application_->copy_record(completion.key, application_record) ||
		application_record.generation != completion.generation) {
		return true;
	}
	if (completion.incremental_edit && edit_journal_store_ &&
		!edit_journal_store_->journal().revision_affects_density(
			record->world_revision
		)) {
		std::shared_ptr<const WtCollisionPayload> previous =
			resource_cache_->find_collision_predecessor(
				completion.key, completion.generation
			);
		if (!previous && application_record.collision_generation.value != 0 &&
				application_record.collision_generation.value < completion.generation.value) {
			previous = resource_cache_->find_collision(
				completion.key, application_record.collision_generation
			);
		}
		if (previous) {
			collision = std::make_shared<WtCollisionPayload>(*previous);
			collision->generation = completion.generation;
			collision->dirty_block_mask = kWtCollisionAllBlocksMask;
			collision->incremental_patch = true;
			auto cached = std::make_shared<WtCollisionPayload>(*collision);
			cached->incremental_patch = false;
			if (resource_cache_->insert_collision(cached, record->generation) !=
					WtChunkResourceCacheStatus::Ok) {
				set_failure(
					WtReadOnlyRuntimeStatus::PipelineTerrainMeshCompletionFailure
				);
				return false;
			}
			causal_trace_.record(
				WtCausalTraceEventKind::CollisionPayloadPrepared,
				WtCausalTraceThreadRole::Runtime,
				&completion.key,
				completion.generation,
				record->world_revision,
				collision->metrics.output_triangles,
				0,
				collision->dirty_block_mask
			);
			return true;
		}
		collision->key = completion.key;
		collision->generation = completion.generation;
		collision->world_origin = wt_chunk_bounds(completion.key).minimum;
		collision->dirty_block_mask = kWtCollisionAllBlocksMask;
		collision->incremental_patch = true;
		collision->regular_only = true;
		collision->preserve_existing = true;
		causal_trace_.record(
			WtCausalTraceEventKind::CollisionPayloadPrepared,
			WtCausalTraceThreadRole::Runtime,
			&completion.key,
			completion.generation,
			record->world_revision,
			0,
			0,
			collision->dirty_block_mask
		);
		return true;
	}
	const WtCollisionBuildStatus collision_status =
		completion.incremental_edit && completion.key.lod == 0 ?
			wt_build_regular_collision_patch(
				*completion.mesh,
				completion.generation,
				collision_policy,
				completion.dirty_regular_brick_mask,
				*collision
			) :
			wt_build_regular_collision_payload(
				*completion.mesh,
				completion.generation,
				collision_policy,
				*collision
			);
	std::shared_ptr<const WtCollisionPayload> cached_collision = collision;
	if (collision_status == WtCollisionBuildStatus::Ok &&
		collision->incremental_patch &&
		collision->dirty_block_mask != kWtCollisionAllBlocksMask) {
		std::shared_ptr<const WtCollisionPayload> previous =
			resource_cache_->find_collision_predecessor(
				completion.key, completion.generation
			);
		const bool predecessor_is_physics_active = previous != nullptr;
		if (!previous && application_record.collision_generation.value != 0 &&
				application_record.collision_generation.value < completion.generation.value) {
			previous = resource_cache_->find_collision(
				completion.key, application_record.collision_generation
			);
		}
		if (previous) {
			auto merged = std::make_shared<WtCollisionPayload>();
			if (wt_merge_collision_patch(*previous, *collision, *merged) !=
					WtCollisionBuildStatus::Ok) {
				set_failure(
					WtReadOnlyRuntimeStatus::PipelineTerrainMeshCompletionFailure
				);
				return false;
			}
			// A queued patch is only valid against the collision generation that is
			// already applied in Godot. Rapid edits can supersede queued generations;
			// the application service then correctly drops those stale entries, but a
			// later patch must not assume their blocks reached the sink. Publish the
			// complete merged generation whenever its predecessor is not applied.
			if (!predecessor_is_physics_active) {
				merged->incremental_patch = true;
				collision = merged;
				++metrics_.collision_unpublished_base_full_rebases;
			}
			auto cached_merged = std::make_shared<WtCollisionPayload>(*merged);
			cached_merged->incremental_patch = false;
			cached_collision = std::move(cached_merged);
		} else {
			if (completion.collision_patch_mesh_only) {
				set_failure(
					WtReadOnlyRuntimeStatus::PipelineTerrainMeshCompletionFailure
				);
				return false;
			}
			// A moving edit can reach a chunk before its first collision payload
			// becomes resident. An incremental block patch has no authoritative
			// base in that case and the physics sink must reject it. The mixed
			// generation already produced the complete CPU mesh, so establish one
			// full base now; subsequent edits return to block replacement.
			auto complete = std::make_shared<WtCollisionPayload>();
			if (wt_build_regular_collision_payload(
					*completion.mesh, completion.generation,
					collision_policy, *complete) != WtCollisionBuildStatus::Ok) {
				set_failure(
					WtReadOnlyRuntimeStatus::PipelineTerrainMeshCompletionFailure
				);
				return false;
			}
			complete->incremental_patch = true;
			collision = complete;
			auto cached_complete = std::make_shared<WtCollisionPayload>(*complete);
			cached_complete->incremental_patch = false;
			cached_collision = std::move(cached_complete);
		}
	}
	if ((!completion.incremental_edit && resource_cache_->insert_mesh(
			completion.mesh,
			completion.generation,
			record->generation
		) != WtChunkResourceCacheStatus::Ok) ||
		collision_status != WtCollisionBuildStatus::Ok ||
		(cached_collision && resource_cache_->insert_collision(
			cached_collision, record->generation
		) != WtChunkResourceCacheStatus::Ok)) {
		set_failure(
			WtReadOnlyRuntimeStatus::PipelineTerrainMeshCompletionFailure
		);
		return false;
	}
	causal_trace_.record(
		WtCausalTraceEventKind::CollisionPayloadPrepared,
		WtCausalTraceThreadRole::Runtime,
		&completion.key,
		completion.generation,
		record->world_revision,
		collision->metrics.output_triangles,
		0,
		collision->dirty_block_mask
	);
	return true;
}

bool WtReadOnlyWorldRuntime::process_terrain_mesh_completion(
	const WtTerrainMeshCompletion &completion
) {
	std::shared_ptr<WtCollisionPayload> collision;
	if (!prepare_terrain_collision_payload(completion, collision)) {
		return false;
	}
	if (!collision) {
		return true;
	}
	WtChunkApplicationRecord application_record;
	if (!application_->copy_record(completion.key, application_record) ||
		application_record.generation != completion.generation) {
		return true;
	}
	WtReadOnlyPublication collision_publication;
	collision_publication.kind = WtReadOnlyPublicationKind::CollisionPayload;
	collision_publication.key = collision->key;
	collision_publication.generation = collision->generation;
	collision_publication.collision_required = true;
	collision_publication.collision = collision;
	collision_publication.interaction_critical =
		application_record.independently_publishable_replacement ||
		is_interaction_critical_key(completion.key);
	if (application_record.collision_required &&
		!push_publication(std::move(collision_publication))) {
		if (!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
		return false;
	}
	return true;
}

bool WtReadOnlyWorldRuntime::process_mesh_completions() {
	bool progressed = false;
	WtPageMeshCompletion completion;
	while (page_runtime_->pop_mesh_completion(completion)) {
		progressed = true;
		causal_trace_.record(
			WtCausalTraceEventKind::MeshCompletionConsumed,
			WtCausalTraceThreadRole::Runtime,
			&completion.key,
			completion.generation
		);
		if (completion.mesh && completion.mesh->transition_mask != 0) {
			causal_trace_.record(
				WtCausalTraceEventKind::TransitionMeshCompletionConsumed,
				WtCausalTraceThreadRole::Runtime,
				&completion.key,
				completion.generation,
				0,
				completion.mesh->transition_mask
			);
		}
		const WtChunkRecord *record = scheduler_->find_record(completion.key);
		if (record == nullptr || record->generation != completion.generation ||
			!completion.mesh || !completion.water_mesh) {
			continue;
		}
		WtChunkApplicationRecord application_record;
		if (!application_->copy_record(completion.key, application_record) ||
			application_record.generation != completion.generation) {
			continue;
		}
		if (!application_record.visual_required) {
			if (completion.gpu_resident_visual_only) {
				auto render = std::make_shared<WtRenderPayload>();
				render->key = completion.key;
				render->generation = completion.generation;
				render->world_origin = completion.mesh->world_origin;
				render->transition_mask = completion.mesh->transition_mask;
				render->publication_source =
					WtRenderPublicationSource::GpuResidentPlaceholder;
				if (resource_cache_->insert_render(render, record->generation) !=
						WtChunkResourceCacheStatus::Ok) {
					set_failure(
						WtReadOnlyRuntimeStatus::PipelineRenderCompletionFailure
					);
					break;
				}
			}
			continue;
		}
		std::shared_ptr<WtCollisionPayload> replacement_collision;
		if (application_record.staged_replacement &&
			application_record.collision_required &&
			!completion.collision_completed_early &&
			!prepare_terrain_collision_payload(
				{
					completion.key,
					completion.generation,
					completion.mesh,
					completion.incremental_edit,
					completion.dirty_regular_brick_mask,
					false,
				},
				replacement_collision
			)) {
			break;
		}
		const WtLodMapEntry *entry = find_plan_entry(
			current_plan_.entries,
			completion.key
		);
		const std::uint8_t render_transition_mask =
			entry != nullptr ? entry->transition_mask :
				completion.mesh->transition_mask;
		if (render_transition_mask != completion.mesh->transition_mask) {
			const WtApplicationStatus supersede_status =
				application_->supersede_visual_generation(
					completion.key,
					completion.generation
				);
			if (supersede_status != WtApplicationStatus::Ok &&
				supersede_status != WtApplicationStatus::AlreadyCurrent &&
				supersede_status != WtApplicationStatus::StaleGeneration) {
				set_failure(
					WtReadOnlyRuntimeStatus::PipelineRenderCompletionFailure
				);
				break;
			}
			const WtDesiredChunk *desired = desired_->find_desired(
				completion.key
			);
			if (desired != nullptr) {
				queue_transition_remeshes({ *desired });
			}
			continue;
		}
		if (completion.gpu_resident_visual_only) {
			auto render = std::make_shared<WtRenderPayload>();
			render->key = completion.key;
			render->generation = completion.generation;
			render->world_origin = completion.mesh->world_origin;
			render->transition_mask = completion.mesh->transition_mask;
			render->publication_source =
				WtRenderPublicationSource::GpuResidentPlaceholder;
			if (resource_cache_->insert_render(render, record->generation) !=
					WtChunkResourceCacheStatus::Ok) {
				set_failure(
					WtReadOnlyRuntimeStatus::PipelineRenderCompletionFailure
				);
				break;
			}
			WtReadOnlyPublication publication;
			publication.kind = WtReadOnlyPublicationKind::RenderPayload;
			publication.key = render->key;
			publication.generation = render->generation;
			publication.render = render;
			publication.staged_replacement =
				application_record.staged_replacement;
			publication.interaction_critical =
				application_record.independently_publishable_replacement ||
				is_interaction_critical_key(completion.key);
			WtReadOnlyPublication collision_publication;
			collision_publication.kind = WtReadOnlyPublicationKind::CollisionPayload;
			if (replacement_collision) {
				collision_publication.key = replacement_collision->key;
				collision_publication.generation = replacement_collision->generation;
				collision_publication.collision_required = true;
				collision_publication.collision = replacement_collision;
				collision_publication.interaction_critical =
					application_record.independently_publishable_replacement ||
					is_interaction_critical_key(completion.key);
			}
			if (application_record.staged_replacement &&
				application_record.collision_required && replacement_collision &&
				!push_publication(std::move(collision_publication))) {
				if (!stop_requested_.load()) {
					set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
				}
				break;
			}
			if (!push_publication(std::move(publication))) {
				if (!stop_requested_.load()) {
					set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
				}
				break;
			}
			std::lock_guard<std::mutex> lock(metrics_mutex_);
			++metrics_.mesh_completions;
			if (completion.mesh->transition_mask != 0) {
				++metrics_.transition_mesh_completions;
			}
			continue;
		}
		auto render = std::make_shared<WtRenderPayload>();
		if (resource_cache_->insert_mesh(
				completion.mesh,
				completion.water_mesh,
				completion.generation,
				record->generation
			) != WtChunkResourceCacheStatus::Ok) {
			set_failure(WtReadOnlyRuntimeStatus::PipelineRenderCompletionFailure);
			break;
		}
		const WtRenderBuildStatus render_status = wt_build_render_payload(
				*completion.mesh,
				*completion.water_mesh,
				completion.generation,
				render_transition_mask,
				*render
			);
		if (render_status != WtRenderBuildStatus::Ok ||
			resource_cache_->insert_render(render, record->generation) !=
				WtChunkResourceCacheStatus::Ok) {
			set_failure(WtReadOnlyRuntimeStatus::PipelineRenderCompletionFailure);
			break;
		}
		WtReadOnlyPublication publication;
		publication.kind = WtReadOnlyPublicationKind::RenderPayload;
		publication.key = render->key;
		publication.generation = render->generation;
		publication.render = render;
		publication.staged_replacement = application_record.staged_replacement;
		publication.interaction_critical =
			application_record.independently_publishable_replacement ||
			is_interaction_critical_key(completion.key);
		WtReadOnlyPublication collision_publication;
		collision_publication.kind = WtReadOnlyPublicationKind::CollisionPayload;
		if (replacement_collision) {
			collision_publication.key = replacement_collision->key;
			collision_publication.generation = replacement_collision->generation;
			collision_publication.collision_required = true;
			collision_publication.collision = replacement_collision;
			collision_publication.interaction_critical =
				application_record.independently_publishable_replacement ||
				is_interaction_critical_key(completion.key);
		}
		if (application_record.staged_replacement &&
			application_record.collision_required && replacement_collision &&
			!push_publication(std::move(collision_publication))) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			break;
		}
		if (!push_publication(std::move(publication))) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			break;
		}
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.mesh_completions;
		if (completion.mesh->transition_mask != 0) {
			++metrics_.transition_mesh_completions;
		}
	}
	return progressed;
}

bool WtReadOnlyWorldRuntime::process_collision_readiness_repairs() {
	if (!desired_) return false;
	{
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.collision_readiness_repair_passes;
	}
	if (has_publication_backlog()) {
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.collision_readiness_repair_publication_blocks;
		return false;
	}
	if (application_->queued_collision_count() != 0 ||
		application_->deferred_collision_count() != 0) {
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.collision_readiness_repair_application_blocks;
		return false;
	}
	const WtSchedulerMetrics scheduler_metrics = scheduler_->get_metrics();
	const WtPageMeshingRuntimeMetrics page_metrics = page_runtime_->get_metrics();
	if (scheduler_->queued_job_count() != 0 ||
		scheduler_->queued_completion_count() != 0 ||
		scheduler_metrics.sampling_records != 0 ||
		scheduler_metrics.meshing_records != 0 ||
		page_metrics.mesh_worker_queued_jobs != 0 ||
		page_metrics.mesh_worker_active_jobs != 0 ||
		page_metrics.mesh_worker_queued_completions != 0) {
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.collision_readiness_repair_pipeline_blocks;
		return false;
	}
	const WtCollisionPolicy collision_policy {
		kWtDefaultCollisionThinRatioSquared,
		config_.collision_activation_distance,
		config_.collision_deactivation_distance,
	};
	collision_readiness_repair_attempts_.erase(
		std::remove_if(
			collision_readiness_repair_attempts_.begin(),
			collision_readiness_repair_attempts_.end(),
			[this](const CollisionReadinessRepairAttempt &attempt) {
				WtChunkApplicationRecord record;
				return !application_->copy_record(attempt.key, record) ||
					record.generation != attempt.generation ||
					!record.collision_required ||
					(record.collision_ready &&
						record.collision_generation == record.generation);
			}
		),
		collision_readiness_repair_attempts_.end()
	);
	const auto repair_already_published = [this](
		const WtChunkKey &key,
		WtGenerationToken generation
	) {
		return std::find_if(
			collision_readiness_repair_attempts_.begin(),
			collision_readiness_repair_attempts_.end(),
			[&](const CollisionReadinessRepairAttempt &attempt) {
				return attempt.key == key && attempt.generation == generation;
			}
		) != collision_readiness_repair_attempts_.end();
	};
	bool progressed = false;
	std::size_t repairs = 0;
	constexpr std::size_t kMaxCollisionRepairsPerPass = 8;
	for (const WtChunkApplicationRecord &record : application_->get_records()) {
		if (!record.collision_required ||
			(record.collision_ready &&
				record.collision_generation == record.generation)) {
			continue;
		}
		const WtDesiredChunk *desired = desired_->find_desired(record.key);
		const bool obsolete_collision_demand =
			(desired != nullptr && !desired->collision_required) ||
			(desired == nullptr && !collision_viewers_.empty());
		if (obsolete_collision_demand) {
			// A prior payload/remesh attempt only deduplicates work for an active
			// collision demand. It must never suppress removal of collision demand
			// after the viewer moves away. Otherwise the obsolete application record
			// remains collision-required forever and blocks its regional retirement.
			if (!push_publication({
					WtReadOnlyPublicationKind::SetCollisionRequired,
					record.key,
					record.generation,
					false,
					{},
					{},
			})) {
				if (!stop_requested_.load()) {
					set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
				}
				return false;
			}
			progressed = true;
			collision_readiness_repair_attempts_.push_back({
				record.key,
				record.generation,
			});
			{
				std::lock_guard<std::mutex> lock(metrics_mutex_);
				++metrics_.collision_readiness_repair_obsolete_clears;
			}
			++repairs;
			if (repairs >= kMaxCollisionRepairsPerPass) break;
			continue;
		}
		if (repair_already_published(record.key, record.generation)) {
			std::lock_guard<std::mutex> lock(metrics_mutex_);
			++metrics_.collision_readiness_repair_duplicate_skips;
			continue;
		}
		const WtChunkRecord *chunk_record = scheduler_->find_record(record.key);
		if (chunk_record == nullptr ||
			chunk_record->generation != record.generation) {
			continue;
		}
		std::shared_ptr<const WtCollisionPayload> collision;
		const WtChunkResourceCacheStatus status =
			resource_cache_->find_or_rebuild_collision(
				record.key,
				record.generation,
				collision_policy,
				collision,
				true
			);
		if (status != WtChunkResourceCacheStatus::Ok &&
			status != WtChunkResourceCacheStatus::NotFound) {
			set_failure(WtReadOnlyRuntimeStatus::PipelineCollisionRepairFailure);
			return false;
		}
		if (!collision) {
			// GPU-only visual generations intentionally omit CPU topology. When
			// such a chunk enters the collision footprint, stage one collision-
			// required generation through the normal remesh path.
			if (status == WtChunkResourceCacheStatus::NotFound &&
				desired != nullptr && desired->collision_required) {
				queue_transition_remeshes({ *desired });
				collision_readiness_repair_attempts_.push_back({
					record.key,
					record.generation,
				});
				progressed = true;
				++repairs;
				if (repairs >= kMaxCollisionRepairsPerPass) break;
			}
			continue;
		}
		WtReadOnlyPublication collision_publication;
		collision_publication.kind =
			WtReadOnlyPublicationKind::CollisionPayload;
		collision_publication.key = collision->key;
		collision_publication.generation = collision->generation;
		collision_publication.collision_required = true;
		collision_publication.collision = collision;
		collision_publication.interaction_critical =
			is_interaction_critical_key(record.key);
		if (!push_publication(std::move(collision_publication))) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			return false;
		}
		progressed = true;
		collision_readiness_repair_attempts_.push_back({
			record.key,
			record.generation,
		});
		++repairs;
		if (repairs >= kMaxCollisionRepairsPerPass) break;
	}
	return progressed;
}

bool WtReadOnlyWorldRuntime::process_visual_readiness_repairs() {
	if (!desired_) return false;
	if (has_publication_backlog() || application_->queued_render_count() != 0) {
		return false;
	}
	const WtSchedulerMetrics scheduler_metrics = scheduler_->get_metrics();
	const WtPageMeshingRuntimeMetrics page_metrics = page_runtime_->get_metrics();
	if (scheduler_->queued_job_count() != 0 ||
		scheduler_->queued_completion_count() != 0 ||
		scheduler_metrics.sampling_records != 0 ||
		scheduler_metrics.meshing_records != 0 ||
		page_metrics.mesh_worker_queued_jobs != 0 ||
		page_metrics.mesh_worker_active_jobs != 0 ||
		page_metrics.mesh_worker_queued_completions != 0) {
		return false;
	}
	bool progressed = false;
	std::size_t repairs = 0;
	readiness_repair_attempts_.erase(
		std::remove_if(
			readiness_repair_attempts_.begin(),
			readiness_repair_attempts_.end(),
			[this](const ReadinessRepairAttempt &attempt) {
				WtChunkApplicationRecord record;
				return !application_->copy_record(attempt.key, record) ||
					record.generation != attempt.generation ||
					!record.staged_replacement;
			}
		),
		readiness_repair_attempts_.end()
	);
	readiness_repair_remesh_attempts_.erase(
		std::remove_if(
			readiness_repair_remesh_attempts_.begin(),
			readiness_repair_remesh_attempts_.end(),
			[this](const ReadinessRepairRemeshAttempt &attempt) {
				WtChunkApplicationRecord record;
				return !application_->copy_record(attempt.key, record) ||
					!record.staged_replacement ||
					record.generation != attempt.generation;
			}
		),
		readiness_repair_remesh_attempts_.end()
	);
	const auto find_attempt = [this](
		const WtChunkKey &key,
		WtGenerationToken generation
	) -> ReadinessRepairAttempt * {
		for (ReadinessRepairAttempt &attempt :
				readiness_repair_attempts_) {
			if (attempt.key == key && attempt.generation == generation) {
				return &attempt;
			}
		}
		return nullptr;
	};
	const auto ensure_attempt = [&](
		const WtChunkKey &key,
		WtGenerationToken generation
	) -> ReadinessRepairAttempt & {
		if (ReadinessRepairAttempt *attempt = find_attempt(key, generation)) {
			return *attempt;
		}
		readiness_repair_attempts_.push_back({ key, generation });
		return readiness_repair_attempts_.back();
	};
	const auto remesh_already_requested = [this](
		const WtChunkKey &key,
		WtGenerationToken generation
	) {
		return std::find_if(
			readiness_repair_remesh_attempts_.begin(),
			readiness_repair_remesh_attempts_.end(),
			[&](const ReadinessRepairRemeshAttempt &attempt) {
				return attempt.key == key && attempt.generation == generation;
			}
		) != readiness_repair_remesh_attempts_.end();
	};
	enum class RepairResult {
		Skipped,
		Waiting,
		Repaired,
		CapacityBlocked,
		Failed,
	};
	const auto process_item = [&](const WtDesiredChunk &item) -> RepairResult {
		if (repairs >= 64U || scheduler_->available_job_capacity() == 0) {
			return RepairResult::CapacityBlocked;
		}
		if (!item.visual_required) return RepairResult::Skipped;
		const WtChunkRecord *record = scheduler_->find_record(item.key);
		if (record == nullptr || record->lifecycle != WtChunkLifecycle::Ready) {
			return RepairResult::Waiting;
		}
		WtChunkApplicationRecord application_record;
		const bool staged_replacement =
			application_->copy_record(item.key, application_record) &&
			application_record.generation == record->generation &&
			application_record.staged_replacement;
		if (staged_replacement) {
			const auto render =
				resource_cache_->find_render(item.key, record->generation);
			if (!render) {
				if (remesh_already_requested(item.key, record->generation)) {
					return RepairResult::Skipped;
				}
				const WtSchedulerStatus scheduler_status =
					scheduler_->request_chunk_version(
						item.key,
						storage_.source_revision(),
						world_revision_.load(),
						item.priority,
						true
					);
				if (scheduler_status == WtSchedulerStatus::JobQueueFull) {
					return RepairResult::CapacityBlocked;
				}
				if (scheduler_status != WtSchedulerStatus::Ok) {
					set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
					return RepairResult::Failed;
				}
				record = scheduler_->find_record(item.key);
				if (record != nullptr) {
					causal_trace_.record(
						WtCausalTraceEventKind::ReadinessRepairGenerationCreated,
						WtCausalTraceThreadRole::Runtime,
						&item.key,
						record->generation,
						world_revision_.load(),
						1
					);
				}
				const WtApplicationStatus application_status =
					record == nullptr ? WtApplicationStatus::NotFound :
					application_->expect_chunk(
						item.key,
						record->generation,
						item.collision_required,
						item.visual_required,
						true,
						item.collision_required,
						record->world_revision
					);
				if (application_status != WtApplicationStatus::Ok &&
					application_status != WtApplicationStatus::AlreadyCurrent) {
					set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
					return RepairResult::Failed;
				}
				WtReadOnlyPublication publication;
				publication.kind = WtReadOnlyPublicationKind::ExpectChunk;
				publication.key = item.key;
				publication.generation = record->generation;
				publication.world_revision = record->world_revision;
				publication.collision_required = item.collision_required;
				publication.visual_required = item.visual_required;
				publication.staged_replacement = true;
				publication.preserve_collision_ready = item.collision_required;
				if (!push_publication(std::move(publication))) {
					if (!stop_requested_.load()) {
						set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
					}
					return RepairResult::Failed;
				}
				readiness_repair_remesh_attempts_.push_back({
					item.key,
					record->generation,
				});
				++repairs;
				progressed = true;
				return RepairResult::Repaired;
			}
			ReadinessRepairAttempt &attempt =
				ensure_attempt(item.key, record->generation);
			if (attempt.render_republished) {
				return RepairResult::Skipped;
			}
			WtReadOnlyPublication publication;
			publication.kind = WtReadOnlyPublicationKind::RenderPayload;
			publication.key = render->key;
			publication.generation = render->generation;
			publication.render = render;
			publication.staged_replacement = true;
			if (!push_publication(std::move(publication))) {
				if (!stop_requested_.load()) {
					set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
				}
				return RepairResult::Failed;
			}
			attempt.render_republished = true;
			++repairs;
			progressed = true;
			return RepairResult::Repaired;
		}
		if (resource_cache_->find_render(item.key, record->generation)) {
			return RepairResult::Skipped;
		}
		const WtSchedulerStatus scheduler_status =
			scheduler_->request_chunk_version(
				item.key,
				storage_.source_revision(),
				world_revision_.load(),
				item.priority,
				true
			);
		if (scheduler_status == WtSchedulerStatus::JobQueueFull) {
			return RepairResult::CapacityBlocked;
		}
		if (scheduler_status != WtSchedulerStatus::Ok) {
			set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
			return RepairResult::Failed;
		}
		record = scheduler_->find_record(item.key);
		if (record != nullptr) {
			causal_trace_.record(
				WtCausalTraceEventKind::ReadinessRepairGenerationCreated,
				WtCausalTraceThreadRole::Runtime,
				&item.key,
				record->generation,
				world_revision_.load(),
				0
			);
		}
		const WtApplicationStatus application_status =
			record == nullptr ? WtApplicationStatus::NotFound :
			application_->expect_chunk(
				item.key,
				record->generation,
				item.collision_required,
				item.visual_required,
				false,
				false,
				record->world_revision
			);
		if (application_status != WtApplicationStatus::Ok &&
			application_status != WtApplicationStatus::AlreadyCurrent) {
			set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
			return RepairResult::Failed;
		}
		WtReadOnlyPublication publication;
		publication.kind = WtReadOnlyPublicationKind::ExpectChunk;
		publication.key = item.key;
		publication.generation = record->generation;
		publication.world_revision = record->world_revision;
		publication.collision_required = item.collision_required;
		publication.visual_required = item.visual_required;
		publication.staged_replacement = false;
		publication.preserve_collision_ready = false;
		if (!push_publication(std::move(publication))) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			return RepairResult::Failed;
		}
		++repairs;
		progressed = true;
		return RepairResult::Repaired;
	};
	for (std::size_t index = 0;
			index < readiness_repair_candidate_keys_.size();) {
		if (repairs >= 64U || scheduler_->available_job_capacity() == 0) {
			break;
		}
		const WtDesiredChunk *desired =
			desired_->find_desired(readiness_repair_candidate_keys_[index]);
		if (desired == nullptr || !desired->visual_required) {
			readiness_repair_candidate_keys_.erase(
				readiness_repair_candidate_keys_.begin() + index
			);
			continue;
		}
		const RepairResult result = process_item(*desired);
		if (result == RepairResult::Failed) {
			return progressed;
		}
		if (result == RepairResult::CapacityBlocked) {
			break;
		}
		if (result == RepairResult::Waiting) {
			++index;
			continue;
		}
		readiness_repair_candidate_keys_.erase(
			readiness_repair_candidate_keys_.begin() + index
		);
	}
	for (const WtDesiredChunk &item : desired_->get_desired_chunks()) {
		if (repairs >= 64U || scheduler_->available_job_capacity() == 0) {
			break;
		}
		const RepairResult result = process_item(item);
		if (result == RepairResult::Failed) {
			return progressed;
		}
		if (result == RepairResult::CapacityBlocked) {
			break;
		}
	}
	return progressed;
}
} // namespace world_transvoxel
