#include "services/wt_page_meshing_runtime.h"

#include "storage/wt_async_storage_service.h"
#include "storage/wt_storage_page_cache.h"

#include <algorithm>

namespace world_transvoxel {
namespace {

void record_storage_failure_key(
	WtPageMeshingRuntimeMetrics &metrics,
	const WtChunkKey &key
) noexcept {
	metrics.last_failure_key_x = key.x;
	metrics.last_failure_key_y = key.y;
	metrics.last_failure_key_z = key.z;
	metrics.last_failure_key_lod = key.lod;
}

} // namespace

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::accept_storage_completion(
	const WtPageLoadCompletion &completion,
	WtStoragePageCache &cache,
	WtStreamScheduler &scheduler
) {
	if (!valid_ || !cache.valid()) {
		return WtPageMeshingRuntimeStatus::InvalidConfiguration;
	}
	const auto waiting_for_completion =
		[&completion](const Record &record) {
			if (record.phase != WtPageMeshingRuntimePhase::Loading) {
				return false;
			}
			const auto dependency = std::lower_bound(
				record.dependencies.begin(),
				record.dependencies.end(),
				completion.key,
				[](const Dependency &left, const WtChunkKey &right) {
					return left.key < right;
				}
			);
			return dependency != record.dependencies.end() &&
				dependency->key == completion.key &&
				!dependency->page &&
				dependency->request_pending;
		};

	std::size_t waiting_records = 0;
	for (const Record &record : records_) {
		waiting_records += waiting_for_completion(record) ? 1U : 0U;
	}
	if (completion.status != WtPageLoadStatus::Ok) {
		for (std::size_t index = records_.size(); index-- > 0;) {
			if (!waiting_for_completion(records_[index])) {
				continue;
			}
			++metrics_.storage_failures;
			record_storage_failure_key(metrics_, completion.key);
			mark_sample_failure(index, scheduler);
		}
		if (waiting_records == 0) {
			++metrics_.stale_storage_completions;
			return WtPageMeshingRuntimeStatus::CompletionNotOwned;
		}
		return WtPageMeshingRuntimeStatus::StorageRequestFailure;
	}

	if (cache.accept_completion(completion, completion.generation) !=
			WtStoragePageCacheStatus::Ok) {
		for (std::size_t index = records_.size(); index-- > 0;) {
			if (!waiting_for_completion(records_[index])) {
				continue;
			}
			++metrics_.cache_failures;
			record_storage_failure_key(metrics_, completion.key);
			mark_sample_failure(index, scheduler);
		}
		return WtPageMeshingRuntimeStatus::CacheFailure;
	}
	if (waiting_records == 0) {
		// A cancelled first consumer does not invalidate immutable source-page
		// work. Keep the completion cached for any coalesced or future consumer.
		++metrics_.stale_storage_completions;
		return WtPageMeshingRuntimeStatus::CompletionNotOwned;
	}

	bool scheduler_backpressure = false;
	bool cache_failure = false;
	for (std::size_t index = records_.size(); index-- > 0;) {
		if (!waiting_for_completion(records_[index])) {
			continue;
		}
		Record &record = records_[index];
		auto dependency = std::lower_bound(
			record.dependencies.begin(),
			record.dependencies.end(),
			completion.key,
			[](const Dependency &left, const WtChunkKey &right) {
				return left.key < right;
			}
		);
		if (cache.find_or_decode(
				completion.key,
				record.source_revision,
				dependency->page
			) != WtStoragePageCacheStatus::Ok ||
			!dependency->page) {
			++metrics_.cache_failures;
			record_storage_failure_key(metrics_, completion.key);
			mark_sample_failure(index, scheduler);
			cache_failure = true;
			continue;
		}
		dependency->request_pending = false;
		++metrics_.accepted_storage_completions;
		if (std::all_of(
				record.dependencies.begin(),
				record.dependencies.end(),
				[](const Dependency &candidate) {
					return static_cast<bool>(candidate.page);
				}
			)) {
			record.phase = WtPageMeshingRuntimePhase::SampleReady;
			scheduler_backpressure =
				submit_pending_result(index, scheduler) ==
					WtPageMeshingRuntimeStatus::SchedulerBackpressure ||
				scheduler_backpressure;
		}
	}
	update_maximum_pins();
	if (cache_failure) {
		return WtPageMeshingRuntimeStatus::CacheFailure;
	}
	return scheduler_backpressure ?
		WtPageMeshingRuntimeStatus::SchedulerBackpressure :
		WtPageMeshingRuntimeStatus::Ok;
}

} // namespace world_transvoxel
