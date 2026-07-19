#include "services/wt_desired_set_runtime.h"

#include "services/wt_chunk_application.h"
#include "services/wt_chunk_resource_cache.h"
#include "services/wt_page_meshing_runtime_owner.h"
#include "storage/wt_storage_page_cache.h"
#include "streaming/wt_stream_scheduler.h"

#include <algorithm>

namespace world_transvoxel {
namespace {

template <typename Item, typename KeyFunction>
bool canonical_keys(
	const std::vector<Item> &items,
	KeyFunction key_function
) noexcept {
	for (std::size_t index = 0; index < items.size(); ++index) {
		const WtChunkKey key = key_function(items[index]);
		if (!wt_is_valid_chunk_key(key) ||
			(index != 0 && !(key_function(items[index - 1]) < key))) {
			return false;
		}
	}
	return true;
}

bool contains_desired(
	const std::vector<WtDesiredChunk> &items,
	const WtChunkKey &key
) noexcept {
	const auto iterator = std::lower_bound(
		items.begin(),
		items.end(),
		key,
		[](const WtDesiredChunk &item, const WtChunkKey &value) {
			return item.key < value;
		}
	);
	return iterator != items.end() && iterator->key == key;
}

bool contains_key(
	const std::vector<WtChunkKey> &items,
	const WtChunkKey &key
) noexcept {
	return std::binary_search(items.begin(), items.end(), key);
}

} // namespace

WtDesiredSetRuntimeService::WtDesiredSetRuntimeService(
	std::size_t change_capacity
) :
		change_capacity_(change_capacity),
		valid_(change_capacity > 0 &&
			change_capacity <= kWtMaximumDesiredChunkCount) {
}

bool WtDesiredSetRuntimeService::valid() const noexcept {
	return valid_;
}

bool WtDesiredSetRuntimeService::validate_delta(
	const WtDesiredSetDelta &delta
) const noexcept {
	if (!canonical_keys(
			delta.added,
			[](const WtDesiredChunk &item) { return item.key; }
		) ||
		!canonical_keys(
			delta.removed,
			[](const WtChunkKey &item) { return item; }
		) ||
		!canonical_keys(
			delta.updated,
			[](const WtDesiredChunk &item) { return item.key; }
		)) {
		return false;
	}
	for (const WtDesiredChunk &item : delta.added) {
		if (item.supporter_count == 0 ||
			contains_key(delta.removed, item.key) ||
			contains_desired(delta.updated, item.key)) {
			return false;
		}
	}
	for (const WtDesiredChunk &item : delta.updated) {
		if (item.supporter_count == 0 ||
			contains_key(delta.removed, item.key)) {
			return false;
		}
	}
	return true;
}

WtDesiredSetRuntimeStatus WtDesiredSetRuntimeService::apply_delta(
	const WtDesiredSetDelta &delta,
	std::uint64_t source_revision,
	std::uint64_t world_revision,
	WtStreamScheduler &scheduler,
	WtStoragePageCache &page_cache,
	WtChunkResourceCache &resource_cache,
	WtChunkApplicationService &application,
	WtPageMeshingRuntimeOwner *page_meshing_runtime
) {
	++metrics_.delta_attempts;
	if (!valid_) {
		++metrics_.capacity_rejections;
		return WtDesiredSetRuntimeStatus::InvalidConfiguration;
	}
	if (delta.added.size() > change_capacity_ ||
		delta.removed.size() > change_capacity_ - delta.added.size() ||
		delta.updated.size() >
			change_capacity_ - delta.added.size() - delta.removed.size()) {
		++metrics_.capacity_rejections;
		return WtDesiredSetRuntimeStatus::ChangeCapacityExceeded;
	}
	if (!validate_delta(delta)) {
		++metrics_.invalid_deltas;
		return WtDesiredSetRuntimeStatus::InvalidDelta;
	}
	if (delta.added.empty() && delta.removed.empty() && delta.updated.empty()) {
		++metrics_.applied_deltas;
		++metrics_.empty_deltas;
		return WtDesiredSetRuntimeStatus::Ok;
	}

	for (const WtChunkKey &key : delta.removed) {
		const WtChunkRecord *record = scheduler.find_record(key);
		const WtChunkApplicationRecord *application_record =
			application.find_record(key);
		if (record == nullptr || application_record == nullptr ||
			record->generation != application_record->generation) {
			++metrics_.state_rejections;
			return WtDesiredSetRuntimeStatus::RuntimeStateMismatch;
		}
	}
	for (const WtDesiredChunk &item : delta.updated) {
		const WtChunkRecord *record = scheduler.find_record(item.key);
		const WtChunkApplicationRecord *application_record =
			application.find_record(item.key);
		if (record == nullptr || application_record == nullptr ||
			record->generation != application_record->generation) {
			++metrics_.state_rejections;
			return WtDesiredSetRuntimeStatus::RuntimeStateMismatch;
		}
	}
	for (const WtDesiredChunk &item : delta.added) {
		if (scheduler.find_record(item.key) != nullptr ||
			application.find_record(item.key) != nullptr) {
			++metrics_.state_rejections;
			return WtDesiredSetRuntimeStatus::RuntimeStateMismatch;
		}
	}
	if (delta.added.size() >
			scheduler.available_record_capacity() + delta.removed.size() ||
		delta.added.size() >
			application.available_record_capacity() + delta.removed.size()) {
		++metrics_.capacity_rejections;
		return WtDesiredSetRuntimeStatus::RecordCapacityExceeded;
	}
	if (scheduler.available_job_capacity() < delta.added.size()) {
		++metrics_.capacity_rejections;
		return WtDesiredSetRuntimeStatus::JobQueueCapacityExceeded;
	}

	for (const WtChunkKey &key : delta.removed) {
		if (page_meshing_runtime != nullptr) {
			const WtPageMeshingRuntimeOwnerStatus status =
				page_meshing_runtime->release_owned_chunk(key);
			if (status == WtPageMeshingRuntimeOwnerStatus::Ok) {
				++metrics_.released_page_meshing_records;
			} else if (status != WtPageMeshingRuntimeOwnerStatus::NotFound &&
				status != WtPageMeshingRuntimeOwnerStatus::StaleGeneration) {
				++metrics_.page_meshing_runtime_failures;
				return WtDesiredSetRuntimeStatus::PageMeshingRuntimeFailure;
			}
		}
		if (scheduler.forget_chunk(key) != WtSchedulerStatus::Ok) {
			++metrics_.scheduler_failures;
			return WtDesiredSetRuntimeStatus::SchedulerFailure;
		}
		if (application.forget_chunk(key) != WtApplicationStatus::Ok) {
			++metrics_.application_failures;
			return WtDesiredSetRuntimeStatus::ApplicationFailure;
		}
		metrics_.evicted_page_entries += page_cache.erase_key(key);
		metrics_.evicted_resource_entries += resource_cache.erase_key(key);
	}
	for (const WtDesiredChunk &item : delta.updated) {
		const WtChunkRecord *record = scheduler.find_record(item.key);
		const bool interactive_edit_in_flight =
			record != nullptr &&
			record->priority == kWtInteractiveEditPriority &&
			(record->lifecycle == WtChunkLifecycle::Sampling ||
				record->lifecycle == WtChunkLifecycle::Meshing);
		const std::int32_t effective_priority =
			interactive_edit_in_flight ?
				kWtInteractiveEditPriority : item.priority;
		if (page_meshing_runtime != nullptr && record != nullptr) {
			const WtPageMeshingRuntimeOwnerStatus status =
				page_meshing_runtime->reprioritize_owned_chunk(
					item.key,
					record->generation,
					effective_priority
				);
			if (status == WtPageMeshingRuntimeOwnerStatus::Ok) {
				++metrics_.reprioritized_page_meshing_records;
			} else if (status != WtPageMeshingRuntimeOwnerStatus::NotFound &&
				status != WtPageMeshingRuntimeOwnerStatus::StaleGeneration) {
				++metrics_.page_meshing_runtime_failures;
				return WtDesiredSetRuntimeStatus::PageMeshingRuntimeFailure;
			}
		}
		const WtSchedulerStatus scheduler_status =
			scheduler.reprioritize_chunk(item.key, effective_priority);
		if (scheduler_status != WtSchedulerStatus::Ok &&
			scheduler_status != WtSchedulerStatus::AlreadyCurrent) {
			++metrics_.scheduler_failures;
			return WtDesiredSetRuntimeStatus::SchedulerFailure;
		}
		const WtApplicationStatus application_status =
			application.set_collision_required(
				item.key,
				item.collision_required
			);
		if (application_status != WtApplicationStatus::Ok &&
			application_status != WtApplicationStatus::AlreadyCurrent) {
			++metrics_.application_failures;
			return WtDesiredSetRuntimeStatus::ApplicationFailure;
		}
	}
	for (const WtDesiredChunk &item : delta.added) {
		if (scheduler.request_chunk_version(
				item.key,
				source_revision,
				world_revision,
				item.priority
			) != WtSchedulerStatus::Ok) {
			++metrics_.scheduler_failures;
			return WtDesiredSetRuntimeStatus::SchedulerFailure;
		}
		const WtChunkRecord *record = scheduler.find_record(item.key);
		if (record == nullptr ||
			application.expect_chunk(
				item.key,
				record->generation,
				item.collision_required
			) != WtApplicationStatus::Ok) {
			++metrics_.application_failures;
			return WtDesiredSetRuntimeStatus::ApplicationFailure;
		}
	}

	++metrics_.applied_deltas;
	metrics_.added_chunks += delta.added.size();
	metrics_.removed_chunks += delta.removed.size();
	metrics_.updated_chunks += delta.updated.size();
	return WtDesiredSetRuntimeStatus::Ok;
}

std::size_t WtDesiredSetRuntimeService::change_capacity() const noexcept {
	return change_capacity_;
}

WtDesiredSetRuntimeMetrics
WtDesiredSetRuntimeService::get_metrics() const noexcept {
	return metrics_;
}

} // namespace world_transvoxel
