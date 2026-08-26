#include "services/wt_page_meshing_runtime.h"

#include "storage/wt_async_storage_service.h"
#include "storage/wt_storage_page_cache.h"

#include <algorithm>

namespace world_transvoxel {
namespace {

bool pending_result_phase(
	WtPageMeshingRuntimePhase phase
) noexcept {
	return phase == WtPageMeshingRuntimePhase::SampleReady ||
		phase == WtPageMeshingRuntimePhase::SampleFailedReady ||
		phase == WtPageMeshingRuntimePhase::MeshReady ||
		phase == WtPageMeshingRuntimePhase::MeshFailedReady;
}

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

std::size_t WtPageMeshingRuntimeService::flush_scheduler_results(
	WtStreamScheduler &scheduler
) {
	std::size_t submitted = 0;
	for (std::size_t index = 0; index < records_.size();) {
		if (!pending_result_phase(records_[index].phase)) {
			++index;
			continue;
		}
		const std::size_t previous_size = records_.size();
		if (submit_pending_result(index, scheduler) ==
				WtPageMeshingRuntimeStatus::Ok) {
			++submitted;
		}
		if (records_.size() == previous_size) {
			++index;
		}
	}
	return submitted;
}

std::size_t WtPageMeshingRuntimeService::resume_loading_records(
	WtAsyncStorageService &storage,
	WtStoragePageCache &cache,
	WtStreamScheduler &scheduler,
	std::size_t maximum_records
) {
	if (!valid_ || !cache.valid() || !storage.is_open() ||
		maximum_records == 0) {
		return 0;
	}
	const std::size_t candidate_limit = std::min(
		maximum_records,
		records_.size()
	);
	if (loading_retry_candidates_.capacity() < candidate_limit) {
		loading_retry_candidates_.reserve(candidate_limit);
	}
	loading_retry_candidates_.clear();
	for (const Record &record : records_) {
		if (record.phase != WtPageMeshingRuntimePhase::Loading) {
			continue;
		}
		const LoadingRetryCandidate candidate {
			record.key,
			record.priority,
		};
		const auto position = std::lower_bound(
			loading_retry_candidates_.begin(),
			loading_retry_candidates_.end(),
			candidate,
			[](const LoadingRetryCandidate &left,
				const LoadingRetryCandidate &right) {
				if (left.priority != right.priority) {
					return left.priority > right.priority;
				}
				return left.key < right.key;
			}
		);
		if (position == loading_retry_candidates_.end() &&
			loading_retry_candidates_.size() >= candidate_limit) {
			continue;
		}
		loading_retry_candidates_.insert(position, candidate);
		if (loading_retry_candidates_.size() > candidate_limit) {
			loading_retry_candidates_.pop_back();
		}
	}

	std::size_t progressed = 0;
	for (const LoadingRetryCandidate &candidate :
		loading_retry_candidates_) {
		auto record = find_record(candidate.key);
		if (record == records_.end() ||
			record->phase != WtPageMeshingRuntimePhase::Loading) {
			continue;
		}
		const std::size_t record_index = static_cast<std::size_t>(
			record - records_.begin()
		);
		bool made_progress = false;
		const WtPageMeshingRuntimeStatus status = resolve_loading_record(
			record_index,
			storage,
			cache,
			scheduler,
			made_progress
		);
		if (made_progress) {
			++progressed;
		}
		if (status == WtPageMeshingRuntimeStatus::SchedulerBackpressure) {
			break;
		}
	}
	return progressed;
}

bool WtPageMeshingRuntimeService::pop_mesh_completion(
	WtPageMeshCompletion &completion
) {
	completion = {};
	for (Record &record : records_) {
		if (record.phase == WtPageMeshingRuntimePhase::Ready &&
			record.mesh && record.water_mesh) {
			completion = {
				record.key,
				record.generation,
				record.mesh,
				record.water_mesh,
				record.gpu_resident_visual_only,
			};
			record.mesh.reset();
			record.water_mesh.reset();
			return true;
		}
	}
	return false;
}

std::vector<WtPageMeshingRuntimeService::Record>::iterator
WtPageMeshingRuntimeService::find_record(const WtChunkKey &key) noexcept {
	const auto record = std::lower_bound(
		records_.begin(),
		records_.end(),
		key,
		[](const Record &left, const WtChunkKey &right) {
			return left.key < right;
		}
	);
	return record != records_.end() && record->key == key ?
		record : records_.end();
}

std::vector<WtPageMeshingRuntimeService::Record>::const_iterator
WtPageMeshingRuntimeService::find_record(
	const WtChunkKey &key
) const noexcept {
	const auto record = std::lower_bound(
		records_.begin(),
		records_.end(),
		key,
		[](const Record &left, const WtChunkKey &right) {
			return left.key < right;
		}
	);
	return record != records_.end() && record->key == key ?
		record : records_.end();
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::resolve_dependency(
	std::size_t record_index,
	std::size_t dependency_index,
	WtAsyncStorageService &storage,
	WtStoragePageCache &cache,
	WtStreamScheduler &scheduler,
	bool &made_progress
) {
	if (record_index >= records_.size() ||
		dependency_index >= records_[record_index].dependencies.size()) {
		return WtPageMeshingRuntimeStatus::NotFound;
	}
	Record &record = records_[record_index];
	Dependency &dependency = record.dependencies[dependency_index];
	if (dependency.page) {
		return WtPageMeshingRuntimeStatus::Ok;
	}
	const WtStoragePageCacheStatus cache_status = cache.find_or_decode(
		dependency.key,
		record.source_revision,
		dependency.page
	);
	if (cache_status == WtStoragePageCacheStatus::Ok) {
		dependency.request_pending = false;
		++metrics_.dependency_cache_hits;
		made_progress = true;
		return WtPageMeshingRuntimeStatus::Ok;
	}
	if (cache_status != WtStoragePageCacheStatus::NotFound) {
		++metrics_.cache_failures;
		record_storage_failure_key(metrics_, dependency.key);
		mark_sample_failure(record_index, scheduler);
		return WtPageMeshingRuntimeStatus::CacheFailure;
	}
	if (dependency.request_pending &&
		dependency.requested_priority == record.priority) {
		return WtPageMeshingRuntimeStatus::Ok;
	}
	const bool reprioritizing_request = dependency.request_pending;
	++metrics_.dependency_cache_misses;
	const WtAsyncStorageStatus storage_status = storage.request_page(
		dependency.key,
		record.generation,
		record.priority
	);
	if (storage_status == WtAsyncStorageStatus::Ok) {
		dependency.request_pending = true;
		dependency.requested_priority = record.priority;
		++metrics_.dependency_requests;
		made_progress = true;
		return WtPageMeshingRuntimeStatus::Ok;
	}
	if (storage_status == WtAsyncStorageStatus::RequestQueueFull) {
		return WtPageMeshingRuntimeStatus::SchedulerBackpressure;
	}
	if (storage_status == WtAsyncStorageStatus::AlreadyPending) {
		dependency.request_pending = true;
		dependency.requested_priority = record.priority;
		if (reprioritizing_request) {
			++metrics_.dependency_reprioritizations;
			made_progress = true;
		}
		return WtPageMeshingRuntimeStatus::Ok;
	}
	++metrics_.storage_failures;
	record_storage_failure_key(metrics_, dependency.key);
	mark_sample_failure(record_index, scheduler);
	return WtPageMeshingRuntimeStatus::StorageRequestFailure;
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::resolve_loading_record(
	std::size_t record_index,
	WtAsyncStorageService &storage,
	WtStoragePageCache &cache,
	WtStreamScheduler &scheduler,
	bool &made_progress
) {
	if (record_index >= records_.size()) {
		return WtPageMeshingRuntimeStatus::NotFound;
	}
	Record &record = records_[record_index];
	if (record.phase != WtPageMeshingRuntimePhase::Loading) {
		return WtPageMeshingRuntimeStatus::NotReady;
	}
	for (std::size_t dependency_index = 0;
		dependency_index < record.dependencies.size();
		++dependency_index) {
		const WtPageMeshingRuntimeStatus status = resolve_dependency(
			record_index,
			dependency_index,
			storage,
			cache,
			scheduler,
			made_progress
		);
		if (status == WtPageMeshingRuntimeStatus::SchedulerBackpressure) {
			update_maximum_pins();
			return status;
		}
		if (status != WtPageMeshingRuntimeStatus::Ok) {
			return status;
		}
		if (record_index >= records_.size()) {
			return WtPageMeshingRuntimeStatus::NotFound;
		}
	}
	update_maximum_pins();
	if (std::all_of(
			record.dependencies.begin(),
			record.dependencies.end(),
			[](const Dependency &dependency) {
				return static_cast<bool>(dependency.page);
			}
		)) {
		record.phase = WtPageMeshingRuntimePhase::SampleReady;
		made_progress = true;
		return submit_pending_result(record_index, scheduler);
	}
	return WtPageMeshingRuntimeStatus::Ok;
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::submit_pending_result(
	std::size_t record_index,
	WtStreamScheduler &scheduler
) {
	if (record_index >= records_.size() ||
		!pending_result_phase(records_[record_index].phase)) {
		return WtPageMeshingRuntimeStatus::NotReady;
	}
	Record &record = records_[record_index];
	const bool sample =
		record.phase == WtPageMeshingRuntimePhase::SampleReady ||
		record.phase == WtPageMeshingRuntimePhase::SampleFailedReady;
	const bool success =
		record.phase == WtPageMeshingRuntimePhase::SampleReady ||
		record.phase == WtPageMeshingRuntimePhase::MeshReady;
	if (scheduler.submit_completion({
			record.key,
			record.generation,
			sample ? WtChunkJobStage::Sample : WtChunkJobStage::Mesh,
			success,
		}) != WtSchedulerStatus::Ok) {
		++metrics_.scheduler_backpressure;
		return WtPageMeshingRuntimeStatus::SchedulerBackpressure;
	}
	if (record.phase == WtPageMeshingRuntimePhase::SampleReady) {
		record.phase = WtPageMeshingRuntimePhase::AwaitingMesh;
		++metrics_.sample_successes;
	} else if (record.phase == WtPageMeshingRuntimePhase::MeshReady) {
		record.phase = WtPageMeshingRuntimePhase::Ready;
	} else {
		records_.erase(records_.begin() +
			static_cast<std::ptrdiff_t>(record_index));
	}
	return WtPageMeshingRuntimeStatus::Ok;
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::mark_sample_failure(
	std::size_t record_index,
	WtStreamScheduler &scheduler
) {
	if (record_index >= records_.size()) {
		return WtPageMeshingRuntimeStatus::NotFound;
	}
	Record &record = records_[record_index];
	for (Dependency &dependency : record.dependencies) {
		dependency.page.reset();
	}
	record.phase = WtPageMeshingRuntimePhase::SampleFailedReady;
	++metrics_.sample_failures;
	return submit_pending_result(record_index, scheduler);
}

void WtPageMeshingRuntimeService::update_maximum_pins() noexcept {
	metrics_.maximum_pinned_pages = std::max(
		metrics_.maximum_pinned_pages,
		pinned_page_count()
	);
}

} // namespace world_transvoxel
