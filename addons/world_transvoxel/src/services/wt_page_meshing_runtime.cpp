#include "services/wt_page_meshing_runtime.h"

#include "editing/wt_chunk_edit_state.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_chunk_page_sample_source.h"
#include "storage/wt_storage_page_cache.h"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace world_transvoxel {
namespace {

bool pending_result_phase(WtPageMeshingRuntimePhase phase) noexcept {
	return phase == WtPageMeshingRuntimePhase::SampleReady ||
		phase == WtPageMeshingRuntimePhase::SampleFailedReady ||
		phase == WtPageMeshingRuntimePhase::MeshReady ||
		phase == WtPageMeshingRuntimePhase::MeshFailedReady;
}

void record_failure_key(
	WtPageMeshingRuntimeMetrics &metrics,
	const WtChunkKey &key
) noexcept {
	metrics.last_failure_key_x = key.x;
	metrics.last_failure_key_y = key.y;
	metrics.last_failure_key_z = key.z;
	metrics.last_failure_key_lod = key.lod;
}

} // namespace

WtPageMeshingRuntimeService::WtPageMeshingRuntimeService(
	std::size_t record_capacity
) :
		record_capacity_(record_capacity),
		valid_(record_capacity > 0 &&
			record_capacity <= kWtMaximumPageMeshingRuntimeRecords) {
	if (valid_) {
		records_.reserve(record_capacity_);
	}
}

bool WtPageMeshingRuntimeService::valid() const noexcept {
	return valid_;
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::begin_sample_job(
	const WtChunkJob &job,
	std::uint8_t transition_mask,
	WtAsyncStorageService &storage,
	WtStoragePageCache &cache,
	WtStreamScheduler &scheduler
) {
	if (!valid_ || !cache.valid() || !storage.is_open()) {
		return WtPageMeshingRuntimeStatus::InvalidConfiguration;
	}
	if (job.stage != WtChunkJobStage::Sample ||
		!wt_is_valid_chunk_key(job.key) ||
		job.generation.value == 0) {
		return WtPageMeshingRuntimeStatus::InvalidJob;
	}
	if ((transition_mask & 0xc0U) != 0 ||
		(transition_mask != 0 && job.key.lod == 0)) {
		return WtPageMeshingRuntimeStatus::InvalidTransitionMask;
	}
	const WtChunkRecord *scheduler_record = scheduler.find_record(job.key);
	if (scheduler_record == nullptr ||
		scheduler_record->generation != job.generation ||
		scheduler_record->source_revision != job.source_revision ||
		scheduler_record->world_revision != job.world_revision ||
		scheduler_record->lifecycle != WtChunkLifecycle::Sampling) {
		return WtPageMeshingRuntimeStatus::NotReady;
	}

	auto existing = find_record(job.key);
	if (existing != records_.end()) {
		if (existing->generation == job.generation) {
			return WtPageMeshingRuntimeStatus::AlreadyPending;
		}
		if (existing->mesh) {
			++metrics_.discarded_mesh_completions;
		}
		records_.erase(existing);
		++metrics_.cancellations;
	}
	if (records_.size() >= record_capacity_) {
		return WtPageMeshingRuntimeStatus::RecordCapacityExceeded;
	}

	std::vector<WtChunkKey> dependency_keys = { job.key };
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		if ((transition_mask & (1U << face_index)) == 0) {
			continue;
		}
		std::array<WtChunkKey, kWtTransitionSupportPagesPerFace> support{};
		if (!wt_transition_support_page_keys(
				job.key,
				static_cast<WtChunkFace>(face_index),
				support
			)) {
			return WtPageMeshingRuntimeStatus::InvalidTransitionMask;
		}
		dependency_keys.insert(
			dependency_keys.end(),
			support.begin(),
			support.end()
		);
	}
	std::sort(dependency_keys.begin(), dependency_keys.end());
	dependency_keys.erase(
		std::unique(dependency_keys.begin(), dependency_keys.end()),
		dependency_keys.end()
	);
	if (dependency_keys.size() > kWtMaximumPageMeshingDependencies) {
		return WtPageMeshingRuntimeStatus::DependencyCapacityExceeded;
	}

	Record record;
	record.key = job.key;
	record.generation = job.generation;
	record.source_revision = job.source_revision;
	record.world_revision = job.world_revision;
	record.priority = job.priority;
	record.transition_mask = transition_mask;
	record.dependencies.reserve(dependency_keys.size());
	for (const WtChunkKey &key : dependency_keys) {
		record.dependencies.push_back({ key, {} });
	}
	const auto position = std::lower_bound(
		records_.begin(),
		records_.end(),
		record.key,
		[](const Record &left, const WtChunkKey &key) {
			return left.key < key;
		}
	);
	const std::size_t record_index = static_cast<std::size_t>(
		position - records_.begin()
	);
	records_.insert(position, std::move(record));
	++metrics_.sample_jobs;

	bool made_progress = false;
	return resolve_loading_record(
		record_index,
		storage,
		cache,
		scheduler,
		made_progress
	);
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::accept_storage_completion(
	const WtPageLoadCompletion &completion,
	WtStoragePageCache &cache,
	WtStreamScheduler &scheduler
) {
	if (!valid_ || !cache.valid()) {
		return WtPageMeshingRuntimeStatus::InvalidConfiguration;
	}
	auto owner = find_completion_owner(completion);
	if (owner == records_.end()) {
		++metrics_.stale_storage_completions;
		return WtPageMeshingRuntimeStatus::CompletionNotOwned;
	}
	const std::size_t record_index = static_cast<std::size_t>(
		owner - records_.begin()
	);
	if (owner->phase != WtPageMeshingRuntimePhase::Loading) {
		++metrics_.stale_storage_completions;
		return WtPageMeshingRuntimeStatus::StaleCompletion;
	}
	auto dependency = std::lower_bound(
		owner->dependencies.begin(),
		owner->dependencies.end(),
		completion.key,
		[](const Dependency &left, const WtChunkKey &right) {
			return left.key < right;
		}
	);
	if (dependency == owner->dependencies.end() ||
		dependency->key != completion.key || dependency->page) {
		++metrics_.stale_storage_completions;
		return WtPageMeshingRuntimeStatus::StaleCompletion;
	}
	if (completion.status != WtPageLoadStatus::Ok) {
		cache.accept_completion(completion, owner->generation);
		++metrics_.storage_failures;
		record_failure_key(metrics_, completion.key);
		mark_sample_failure(record_index, scheduler);
		return WtPageMeshingRuntimeStatus::StorageRequestFailure;
	}
	if (cache.accept_completion(completion, owner->generation) !=
			WtStoragePageCacheStatus::Ok ||
		cache.find_or_decode(
			completion.key,
			owner->source_revision,
			dependency->page
		) != WtStoragePageCacheStatus::Ok ||
		!dependency->page) {
		++metrics_.cache_failures;
		record_failure_key(metrics_, completion.key);
		mark_sample_failure(record_index, scheduler);
		return WtPageMeshingRuntimeStatus::CacheFailure;
	}
	dependency->request_pending = false;
	++metrics_.accepted_storage_completions;
	update_maximum_pins();
	if (std::all_of(
			owner->dependencies.begin(),
			owner->dependencies.end(),
			[](const Dependency &dependency) {
				return static_cast<bool>(dependency.page);
			}
		)) {
		owner->phase = WtPageMeshingRuntimePhase::SampleReady;
		return submit_pending_result(record_index, scheduler);
	}
	return WtPageMeshingRuntimeStatus::Ok;
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::execute_mesh_job(
	const WtChunkJob &job,
	const WtChunkMesher &mesher,
	WtChunkMeshingScratch &scratch,
	WtStreamScheduler &scheduler,
	const WtEditJournal *edit_journal,
	std::uint64_t initial_world_revision
) {
	if (!valid_) {
		return WtPageMeshingRuntimeStatus::InvalidConfiguration;
	}
	if (job.stage != WtChunkJobStage::Mesh ||
		job.generation.value == 0) {
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
	if (record == records_.end()) {
		return WtPageMeshingRuntimeStatus::NotFound;
	}
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
	auto primary = std::lower_bound(
		record->dependencies.begin(),
		record->dependencies.end(),
		record->key,
		[](const Dependency &left, const WtChunkKey &right) {
			return left.key < right;
		}
	);
	bool source_valid =
		primary != record->dependencies.end() &&
		primary->key == record->key &&
		static_cast<bool>(primary->page);
	if (source_valid && edit_journal != nullptr) {
		for (Dependency &dependency : record->dependencies) {
			WtChunkEditState edit_state;
			if (!dependency.page ||
				edit_state.initialize(
					*dependency.page,
					record->source_revision,
					initial_world_revision
				) != WtChunkEditStatus::Ok ||
				edit_journal->replay_until(
					record->world_revision,
					edit_state
				) != WtEditJournalStatus::Ok ||
				edit_state.current_world_revision() != record->world_revision) {
				source_valid = false;
				break;
			}
			dependency.page = std::make_shared<const WtChunkPage>(
				edit_state.page()
			);
		}
		if (!source_valid) {
			for (Dependency &dependency : record->dependencies) {
				dependency.page.reset();
			}
			record->phase = WtPageMeshingRuntimePhase::MeshFailedReady;
			++metrics_.mesh_failures;
			record_failure_key(metrics_, record->key);
			submit_pending_result(record_index, scheduler);
			return WtPageMeshingRuntimeStatus::EditReplayFailure;
		}
		primary = std::lower_bound(
			record->dependencies.begin(),
			record->dependencies.end(),
			record->key,
			[](const Dependency &left, const WtChunkKey &right) {
				return left.key < right;
			}
		);
	}
	std::unique_ptr<WtChunkPageSampleSource> source;
	if (source_valid) {
		source = std::make_unique<WtChunkPageSampleSource>(*primary->page);
		for (const Dependency &dependency : record->dependencies) {
			if (dependency.key == record->key) {
				continue;
			}
			if (!dependency.page ||
				source->add_transition_support_page(*dependency.page) !=
					WtChunkPageSampleSourceStatus::Ok) {
				source_valid = false;
				break;
			}
		}
		source_valid = source_valid &&
			source->has_transition_support(record->transition_mask);
	}
	auto mesh = std::make_shared<WtChunkMeshResult>();
	const bool mesh_ok = source_valid &&
		mesher.mesh(
			{ record->key, record->transition_mask, 0.0F, 0.25F },
			*source,
			*mesh,
			scratch
		) == WtChunkMeshingStatus::Ok;
	for (Dependency &dependency : record->dependencies) {
		dependency.page.reset();
	}
	if (!mesh_ok) {
		record->phase = WtPageMeshingRuntimePhase::MeshFailedReady;
		++metrics_.mesh_failures;
		record_failure_key(metrics_, record->key);
		submit_pending_result(record_index, scheduler);
		return WtPageMeshingRuntimeStatus::MeshingFailure;
	}
	record->mesh = std::move(mesh);
	record->phase = WtPageMeshingRuntimePhase::MeshReady;
	++metrics_.mesh_successes;
	return submit_pending_result(record_index, scheduler);
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
	std::size_t progressed = 0;
	std::size_t visited = 0;
	for (std::size_t index = 0;
		index < records_.size() && visited < maximum_records;) {
		if (records_[index].phase != WtPageMeshingRuntimePhase::Loading) {
			++index;
			continue;
		}
		++visited;
		const WtChunkKey key = records_[index].key;
		bool made_progress = false;
		const WtPageMeshingRuntimeStatus status = resolve_loading_record(
			index,
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
		if (index < records_.size() && records_[index].key == key) {
			++index;
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
			record.mesh) {
			completion = { record.key, record.generation, record.mesh };
			record.mesh.reset();
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

std::vector<WtPageMeshingRuntimeService::Record>::iterator
WtPageMeshingRuntimeService::find_completion_owner(
	const WtPageLoadCompletion &completion
) noexcept {
	for (auto record = records_.begin(); record != records_.end(); ++record) {
		if (record->generation != completion.generation) {
			continue;
		}
		const auto dependency = std::lower_bound(
			record->dependencies.begin(),
			record->dependencies.end(),
			completion.key,
			[](const Dependency &left, const WtChunkKey &right) {
				return left.key < right;
			}
		);
		if (dependency != record->dependencies.end() &&
			dependency->key == completion.key) {
			return record;
		}
	}
	return records_.end();
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
		record_failure_key(metrics_, dependency.key);
		mark_sample_failure(record_index, scheduler);
		return WtPageMeshingRuntimeStatus::CacheFailure;
	}
	if (dependency.request_pending) {
		return WtPageMeshingRuntimeStatus::Ok;
	}
	++metrics_.dependency_cache_misses;
	const WtAsyncStorageStatus storage_status = storage.request_page(
		dependency.key,
		record.generation,
		record.priority
	);
	if (storage_status == WtAsyncStorageStatus::Ok) {
		dependency.request_pending = true;
		++metrics_.dependency_requests;
		made_progress = true;
		return WtPageMeshingRuntimeStatus::Ok;
	}
	if (storage_status == WtAsyncStorageStatus::RequestQueueFull) {
		return WtPageMeshingRuntimeStatus::SchedulerBackpressure;
	}
	if (storage_status == WtAsyncStorageStatus::AlreadyPending) {
		dependency.request_pending = true;
		return WtPageMeshingRuntimeStatus::Ok;
	}
	++metrics_.storage_failures;
	record_failure_key(metrics_, dependency.key);
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
