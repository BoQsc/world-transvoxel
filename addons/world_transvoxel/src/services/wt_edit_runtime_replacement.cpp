#include "services/wt_edit_runtime_replacement.h"

#include "services/wt_chunk_application.h"
#include "services/wt_chunk_resource_cache.h"
#include "services/wt_page_meshing_runtime_owner.h"
#include "storage/wt_storage_page_cache.h"
#include "streaming/wt_multi_viewer_desired_set.h"
#include "streaming/wt_stream_scheduler.h"

namespace world_transvoxel {

WtEditRuntimeReplacementService::WtEditRuntimeReplacementService(
	std::size_t replacement_capacity
) :
		replacement_capacity_(replacement_capacity),
		valid_(replacement_capacity > 0 &&
			replacement_capacity <= kWtMaximumEditRuntimeReplacements) {
	if (valid_) {
		affected_.reserve(replacement_capacity);
		prepared_.reserve(replacement_capacity);
		last_replacements_.reserve(replacement_capacity);
	}
}

bool WtEditRuntimeReplacementService::valid() const noexcept {
	return valid_;
}

WtEditRuntimeReplacementStatus
WtEditRuntimeReplacementService::prepare_loaded_chunks(
	const WtEditTransaction &transaction,
	const WtEditSpatialIndex &spatial_index,
	const WtStreamScheduler &scheduler,
	const WtChunkApplicationService &application,
	const std::vector<WtDesiredChunk> *desired_chunks
) {
	++metrics_.transaction_attempts;
	affected_.clear();
	prepared_.clear();
	last_replacements_.clear();
	has_prepared_ = false;
	if (!valid_) {
		++metrics_.capacity_rejections;
		return WtEditRuntimeReplacementStatus::InvalidConfiguration;
	}
	if (spatial_index.query_transaction(transaction, affected_) !=
		WtEditSpatialStatus::Ok) {
		++metrics_.spatial_rejections;
		return WtEditRuntimeReplacementStatus::SpatialQueryFailed;
	}
	metrics_.queried_chunks += affected_.size();
	if (affected_.size() > replacement_capacity_) {
		++metrics_.capacity_rejections;
		return WtEditRuntimeReplacementStatus::AffectedCapacityExceeded;
	}
	if (affected_.empty()) {
		prepared_source_revision_ = transaction.source_revision;
		prepared_base_revision_ = transaction.base_revision;
		prepared_committed_revision_ = transaction.committed_revision;
		has_prepared_ = true;
		return WtEditRuntimeReplacementStatus::Ok;
	}

	for (const WtChunkKey &key : affected_) {
		const WtChunkRecord *record = scheduler.find_record(key);
		const WtChunkApplicationRecord *application_record =
			application.find_record(key);
		if (record == nullptr || application_record == nullptr ||
			record->lifecycle == WtChunkLifecycle::Cancelled ||
			application_record->generation != record->generation) {
			++metrics_.state_rejections;
			return WtEditRuntimeReplacementStatus::RuntimeStateMismatch;
		}
		if (record->source_revision != transaction.source_revision) {
			++metrics_.state_rejections;
			return WtEditRuntimeReplacementStatus::SourceRevisionMismatch;
		}
		if (record->world_revision > transaction.base_revision) {
			++metrics_.state_rejections;
			return WtEditRuntimeReplacementStatus::WorldRevisionMismatch;
		}
		bool collision_required = application_record->collision_required;
		bool visual_required = application_record->visual_required;
		if (desired_chunks != nullptr) {
			const WtDesiredChunk *desired = nullptr;
			for (const WtDesiredChunk &candidate : *desired_chunks) {
				if (candidate.key == key) {
					desired = &candidate;
					break;
				}
			}
			if (desired == nullptr) {
				++metrics_.state_rejections;
				return WtEditRuntimeReplacementStatus::RuntimeStateMismatch;
			}
			collision_required = desired->collision_required;
			visual_required = desired->visual_required;
		}
		prepared_.push_back({
			key,
			record->generation,
			record->source_revision,
			record->world_revision,
			collision_required,
			visual_required,
		});
	}
	if (scheduler.available_job_capacity() < prepared_.size()) {
		++metrics_.capacity_rejections;
		return WtEditRuntimeReplacementStatus::JobQueueCapacityExceeded;
	}
	prepared_source_revision_ = transaction.source_revision;
	prepared_base_revision_ = transaction.base_revision;
	prepared_committed_revision_ = transaction.committed_revision;
	has_prepared_ = true;
	return WtEditRuntimeReplacementStatus::Ok;
}

WtEditRuntimeReplacementStatus
WtEditRuntimeReplacementService::apply_prepared(
	const WtEditTransaction &transaction,
	WtStreamScheduler &scheduler,
	WtStoragePageCache &page_cache,
	WtChunkResourceCache &resource_cache,
	WtChunkApplicationService &application,
	WtPageMeshingRuntimeOwner *page_meshing_runtime
) {
	if (!has_prepared_ ||
		transaction.source_revision != prepared_source_revision_ ||
		transaction.base_revision != prepared_base_revision_ ||
		transaction.committed_revision != prepared_committed_revision_) {
		++metrics_.state_rejections;
		return WtEditRuntimeReplacementStatus::RuntimeStateMismatch;
	}
	has_prepared_ = false;
	if (prepared_.empty()) {
		++metrics_.completed_transactions;
		++metrics_.empty_transactions;
		return WtEditRuntimeReplacementStatus::Ok;
	}
	for (const PreparedReplacement &replacement : prepared_) {
		if (page_meshing_runtime != nullptr) {
			const WtPageMeshingRuntimeOwnerStatus status =
				page_meshing_runtime->cancel_owned_generation(
					replacement.key,
					replacement.previous_generation
				);
			if (status == WtPageMeshingRuntimeOwnerStatus::Ok) {
				++metrics_.cancelled_page_meshing_generations;
			} else if (status != WtPageMeshingRuntimeOwnerStatus::NotFound) {
				++metrics_.page_meshing_runtime_failures;
				return WtEditRuntimeReplacementStatus::PageMeshingRuntimeFailure;
			}
		}
		const WtSchedulerStatus scheduler_status =
			scheduler.request_chunk_version(
				replacement.key,
				replacement.source_revision,
				transaction.committed_revision,
				kWtInteractiveEditPriority
			);
		if (scheduler_status != WtSchedulerStatus::Ok) {
			++metrics_.scheduler_failures;
			return WtEditRuntimeReplacementStatus::SchedulerFailure;
		}
		const WtChunkRecord *current = scheduler.find_record(replacement.key);
		if (current == nullptr) {
			++metrics_.scheduler_failures;
			return WtEditRuntimeReplacementStatus::SchedulerFailure;
		}
		if (application.expect_chunk(
				replacement.key,
				current->generation,
				replacement.collision_required,
				replacement.visual_required,
				true,
				replacement.collision_required
			) != WtApplicationStatus::Ok) {
			++metrics_.application_failures;
			return WtEditRuntimeReplacementStatus::ApplicationFailure;
		}

		const std::size_t page_entries = page_cache.erase_key(replacement.key);
		const std::size_t resource_entries =
			resource_cache.erase_key(replacement.key);
		last_replacements_.push_back({
			replacement.key,
			replacement.previous_generation,
			current->generation,
			replacement.source_revision,
			replacement.previous_world_revision,
			transaction.committed_revision,
			page_entries,
			resource_entries,
			replacement.collision_required,
			replacement.visual_required,
		});
		metrics_.evicted_page_entries += page_entries;
		metrics_.evicted_resource_entries += resource_entries;
	}

	++metrics_.completed_transactions;
	metrics_.replaced_chunks += last_replacements_.size();
	return WtEditRuntimeReplacementStatus::Ok;
}

WtEditRuntimeReplacementStatus
WtEditRuntimeReplacementService::replace_loaded_chunks(
	const WtEditTransaction &transaction,
	const WtEditSpatialIndex &spatial_index,
	WtStreamScheduler &scheduler,
	WtStoragePageCache &page_cache,
	WtChunkResourceCache &resource_cache,
	WtChunkApplicationService &application,
	WtPageMeshingRuntimeOwner *page_meshing_runtime,
	const std::vector<WtDesiredChunk> *desired_chunks
) {
	const WtEditRuntimeReplacementStatus prepare = prepare_loaded_chunks(
		transaction, spatial_index, scheduler, application, desired_chunks
	);
	if (prepare != WtEditRuntimeReplacementStatus::Ok) return prepare;
	return apply_prepared(
		transaction,
		scheduler,
		page_cache,
		resource_cache,
		application,
		page_meshing_runtime
	);
}

std::size_t WtEditRuntimeReplacementService::replacement_capacity() const noexcept {
	return replacement_capacity_;
}

const std::vector<WtEditRuntimeReplacementRecord> &
WtEditRuntimeReplacementService::get_last_replacements() const noexcept {
	return last_replacements_;
}

WtEditRuntimeReplacementMetrics
WtEditRuntimeReplacementService::get_metrics() const noexcept {
	return metrics_;
}

} // namespace world_transvoxel
