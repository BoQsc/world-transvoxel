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
	if (resource_cache_->insert_mesh(
			completion.mesh,
			completion.generation,
			record->generation
		) != WtChunkResourceCacheStatus::Ok ||
		wt_build_regular_collision_payload(
			*completion.mesh,
			completion.generation,
			collision_policy,
			*collision
		) != WtCollisionBuildStatus::Ok ||
		resource_cache_->insert_collision(collision, record->generation) !=
			WtChunkResourceCacheStatus::Ok) {
		set_failure(
			WtReadOnlyRuntimeStatus::PipelineTerrainMeshCompletionFailure
		);
		return false;
	}
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
	if (application_record.collision_required &&
		!push_publication({
			WtReadOnlyPublicationKind::CollisionPayload,
			collision->key,
			collision->generation,
			true,
			{},
			collision,
		})) {
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
		const WtChunkRecord *record = scheduler_->find_record(completion.key);
		if (record == nullptr || record->generation != completion.generation ||
			!completion.mesh || !completion.water_mesh) {
			continue;
		}
		WtChunkApplicationRecord application_record;
		if (!application_->copy_record(completion.key, application_record) ||
			application_record.generation != completion.generation ||
			!application_record.visual_required) {
			continue;
		}
		std::shared_ptr<WtCollisionPayload> collision;
		if (application_record.staged_replacement &&
			!prepare_terrain_collision_payload(
				{ completion.key, completion.generation, completion.mesh },
				collision
			)) {
			break;
		}
		auto render = std::make_shared<WtRenderPayload>();
		const WtLodMapEntry *entry = find_plan_entry(
			current_plan_.entries,
			completion.key
		);
		const std::uint8_t render_transition_mask =
			entry != nullptr ? entry->transition_mask :
				completion.mesh->transition_mask;
		if (resource_cache_->insert_mesh(
				completion.mesh,
				completion.water_mesh,
				completion.generation,
				record->generation
			) != WtChunkResourceCacheStatus::Ok) {
			set_failure(WtReadOnlyRuntimeStatus::PipelineRenderCompletionFailure);
			break;
		}
		WtRenderBuildStatus render_status = wt_build_render_payload(
				*completion.mesh,
				*completion.water_mesh,
				completion.generation,
				render_transition_mask,
				*render
			);
		if (render_status != WtRenderBuildStatus::Ok &&
			render_transition_mask != completion.mesh->transition_mask) {
			render_status = wt_build_render_payload(
				*completion.mesh,
				*completion.water_mesh,
				completion.generation,
				completion.mesh->transition_mask,
				*render
			);
			const WtDesiredChunk *desired = desired_->find_desired(
				completion.key
			);
			if (desired != nullptr) {
				queue_transition_remeshes({ *desired });
			}
		}
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
		if (!push_publication(std::move(publication))) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			break;
		}
		if (application_record.staged_replacement &&
			application_record.collision_required && collision &&
			!push_publication({
				WtReadOnlyPublicationKind::CollisionPayload,
				collision->key,
				collision->generation,
				true,
				{},
				collision,
			})) {
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
	if (has_publication_backlog() ||
		application_->queued_collision_count() != 0 ||
		application_->deferred_collision_count() != 0) {
		return false;
	}
	const WtSchedulerMetrics scheduler_metrics = scheduler_->get_metrics();
	if (scheduler_->queued_job_count() != 0 ||
		scheduler_->queued_completion_count() != 0 ||
		scheduler_metrics.sampling_records != 0 ||
		scheduler_metrics.meshing_records != 0) {
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
					record.collision_ready;
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
		if (!record.collision_required || record.collision_ready) continue;
		if (repair_already_published(record.key, record.generation)) continue;
		const WtDesiredChunk *desired = desired_->find_desired(record.key);
		if (desired != nullptr && !desired->collision_required) {
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
			++repairs;
			if (repairs >= kMaxCollisionRepairsPerPass) break;
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
		if (!collision) continue;
		if (!push_publication({
				WtReadOnlyPublicationKind::CollisionPayload,
				collision->key,
				collision->generation,
				true,
				{},
				collision,
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
	if (scheduler_->queued_job_count() != 0 ||
		scheduler_->queued_completion_count() != 0 ||
		scheduler_metrics.sampling_records != 0 ||
		scheduler_metrics.meshing_records != 0) {
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
				if (!application_->copy_record(attempt.key, record) ||
					!record.staged_replacement ||
					record.generation != attempt.generation) {
					return true;
				}
				const WtChunkRecord *scheduler_record =
					scheduler_->find_record(attempt.key);
				return scheduler_record != nullptr &&
					scheduler_record->lifecycle == WtChunkLifecycle::Ready &&
					!resource_cache_->find_render(
						attempt.key,
						attempt.generation
					);
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
				const WtApplicationStatus application_status =
					record == nullptr ? WtApplicationStatus::NotFound :
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
					set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
					return RepairResult::Failed;
				}
				WtReadOnlyPublication publication;
				publication.kind = WtReadOnlyPublicationKind::ExpectChunk;
				publication.key = item.key;
				publication.generation = record->generation;
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
		const WtApplicationStatus application_status =
			record == nullptr ? WtApplicationStatus::NotFound :
			application_->expect_chunk(
				item.key,
				record->generation,
				item.collision_required,
				item.visual_required,
				false,
				false
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
