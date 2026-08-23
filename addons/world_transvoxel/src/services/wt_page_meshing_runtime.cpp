#include "services/wt_page_meshing_runtime.h"

#include "storage/wt_chunk_page_sample_source.h"

#include "services/wt_page_meshing_runtime_internal.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_storage_page_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace world_transvoxel {
namespace {

bool support_pages_available(
	const WtChunkKey &key,
	WtChunkFace face,
	const WtAsyncStorageService &storage,
	std::array<WtChunkKey, kWtTransitionSupportPagesPerFace> &support
) noexcept {
	if (!wt_transition_support_page_keys(key, face, support)) {
		return false;
	}
	for (const WtChunkKey &support_key : support) {
		if (!storage.has_page(support_key)) {
			return false;
		}
	}
	return true;
}

std::uint8_t supported_cached_transition_mask(
	const WtChunkKey &key,
	std::uint8_t transition_mask,
	std::uint8_t requested_cached_transition_mask,
	const WtAsyncStorageService &storage
) noexcept {
	if (key.lod == 0) {
		return 0;
	}
	std::uint8_t cached_transition_mask = transition_mask;
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		const std::uint8_t bit = static_cast<std::uint8_t>(1U << face_index);
		if ((requested_cached_transition_mask & bit) == 0) {
			continue;
		}
		std::array<WtChunkKey, kWtTransitionSupportPagesPerFace> support{};
		if (support_pages_available(
				key,
				static_cast<WtChunkFace>(face_index),
				storage,
				support
			)) {
			cached_transition_mask |= bit;
		}
	}
	return cached_transition_mask;
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

std::uint64_t steady_time_ns() noexcept {
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()
		).count()
	);
}

} // namespace

struct WtPageMeshingRuntimeService::AsyncState {
	explicit AsyncState(
		std::size_t requested_worker_count,
		std::size_t requested_queue_capacity
	) :
			worker_count(requested_worker_count),
			queue_capacity(requested_queue_capacity),
			completion_capacity(requested_queue_capacity) {
		workers.reserve(worker_count);
		for (std::size_t index = 0; index < worker_count; ++index) {
			workers.emplace_back([this]() { worker_main(); });
		}
	}

	~AsyncState() {
		{
			std::lock_guard<std::mutex> lock(work_mutex);
			stopping.store(true, std::memory_order_release);
			work.clear();
		}
		work_available.notify_all();
		completion_space.notify_all();
		for (std::thread &worker : workers) {
			if (worker.joinable()) worker.join();
		}
	}

	bool submit(PreparedMeshJob prepared) {
		std::lock_guard<std::mutex> lock(work_mutex);
		if (stopping.load(std::memory_order_acquire) ||
			work.size() >= queue_capacity) {
			std::lock_guard<std::mutex> metrics_lock(metrics_mutex);
			++metrics.mesh_worker_queue_rejections;
			return false;
		}
		prepared.enqueued_time_ns = steady_time_ns();
		work.push_back(std::move(prepared));
		{
			std::lock_guard<std::mutex> metrics_lock(metrics_mutex);
			++metrics.mesh_worker_accepted_jobs;
			metrics.mesh_worker_queued_jobs = work.size();
		}
		work_available.notify_one();
		return true;
	}

	bool admission_available() const {
		std::lock_guard<std::mutex> lock(work_mutex);
		return !stopping.load(std::memory_order_acquire) &&
			work.size() < worker_count;
	}

	bool pop_completion(PreparedMeshCompletion &completion) {
		std::lock_guard<std::mutex> lock(completion_mutex);
		if (completions.empty()) return false;
		completion = std::move(completions.front());
		completions.erase(completions.begin());
		{
			std::lock_guard<std::mutex> metrics_lock(metrics_mutex);
			metrics.mesh_worker_queued_completions = completions.size();
		}
		completion_space.notify_one();
		return true;
	}

	std::size_t cancel(
		const WtChunkKey &key,
		WtGenerationToken generation
	) {
		std::lock_guard<std::mutex> lock(work_mutex);
		const std::size_t before = work.size();
		work.erase(
			std::remove_if(
				work.begin(),
				work.end(),
				[&](const PreparedMeshJob &item) {
					return item.job.key == key &&
						item.job.generation == generation;
				}
			),
			work.end()
		);
		const std::size_t removed = before - work.size();
		if (removed != 0) {
			std::lock_guard<std::mutex> metrics_lock(metrics_mutex);
			metrics.mesh_worker_cancelled_queued_jobs += removed;
			metrics.mesh_worker_queued_jobs = work.size();
		}
		return removed;
	}

	bool reprioritize(
		const WtChunkKey &key,
		WtGenerationToken generation,
		std::int32_t priority
	) {
		std::lock_guard<std::mutex> lock(work_mutex);
		for (PreparedMeshJob &item : work) {
			if (item.job.key != key || item.job.generation != generation) {
				continue;
			}
			item.job.priority = priority;
			std::lock_guard<std::mutex> metrics_lock(metrics_mutex);
			++metrics.mesh_worker_reprioritized_queued_jobs;
			return true;
		}
		return false;
	}

	void set_notifier(std::function<void()> value) {
		std::lock_guard<std::mutex> lock(notifier_mutex);
		notifier = std::move(value);
	}

	WtPageMeshingRuntimeMetrics snapshot() const {
		std::lock_guard<std::mutex> lock(metrics_mutex);
		WtPageMeshingRuntimeMetrics result = metrics;
		result.mesh_worker_count = worker_count;
		return result;
	}

	void worker_main() {
		WtChunkMesher mesher(wt_get_transvoxel_mit_backend());
		WtChunkMeshingScratch scratch;
		while (true) {
			PreparedMeshJob prepared;
			std::size_t queued_after_pop = 0;
			{
				std::unique_lock<std::mutex> lock(work_mutex);
				work_available.wait(lock, [this]() {
					return stopping.load(std::memory_order_acquire) ||
						!work.empty();
				});
				if (stopping.load(std::memory_order_acquire) && work.empty()) {
					return;
				}
				const auto selected = std::max_element(
					work.begin(),
					work.end(),
					[](const PreparedMeshJob &left, const PreparedMeshJob &right) {
						if (left.job.priority != right.job.priority) {
							return left.job.priority < right.job.priority;
						}
						return left.job.sequence > right.job.sequence;
					}
				);
				prepared = std::move(*selected);
				work.erase(selected);
				queued_after_pop = work.size();
			}
			const std::uint64_t started = steady_time_ns();
			const std::uint64_t queue_wait =
				started - prepared.enqueued_time_ns;
			if (prepared.execution_callback) {
				prepared.execution_callback({
					prepared.job,
					prepared.transition_mask,
					true,
					0,
					WtPageMeshingRuntimeStatus::Ok,
				});
			}
			{
				std::lock_guard<std::mutex> lock(metrics_mutex);
				++metrics.mesh_worker_started_jobs;
				++metrics.mesh_worker_active_jobs;
				metrics.mesh_worker_maximum_active_jobs = std::max(
					metrics.mesh_worker_maximum_active_jobs,
					metrics.mesh_worker_active_jobs
				);
				metrics.mesh_worker_queued_jobs = queued_after_pop;
				metrics.mesh_worker_queue_wait_ns_last = queue_wait;
				metrics.mesh_worker_queue_wait_ns_total += queue_wait;
				metrics.mesh_worker_queue_wait_ns_maximum = std::max(
					metrics.mesh_worker_queue_wait_ns_maximum,
					queue_wait
				);
			}
			PreparedMeshCompletion completion =
				WtPageMeshingRuntimeService::execute_prepared_mesh_job(
					std::move(prepared), mesher, scratch
				);
			completion.queue_wait_ns = queue_wait;
			completion.execute_time_ns = steady_time_ns() - started;
			if (completion.prepared.execution_callback) {
				completion.prepared.execution_callback({
					completion.prepared.job,
					completion.prepared.transition_mask,
					false,
					completion.execute_time_ns,
					completion.status,
				});
			}
			{
				std::lock_guard<std::mutex> lock(metrics_mutex);
				--metrics.mesh_worker_active_jobs;
				++metrics.mesh_worker_completed_jobs;
				metrics.mesh_worker_execute_time_ns_last =
					completion.execute_time_ns;
				metrics.mesh_worker_execute_time_ns_total +=
					completion.execute_time_ns;
				metrics.mesh_worker_execute_time_ns_maximum = std::max(
					metrics.mesh_worker_execute_time_ns_maximum,
					completion.execute_time_ns
				);
			}
			{
				std::unique_lock<std::mutex> lock(completion_mutex);
				completion_space.wait(lock, [this]() {
					return stopping.load(std::memory_order_acquire) ||
						completions.size() < completion_capacity;
				});
				if (stopping.load(std::memory_order_acquire)) return;
				completions.push_back(std::move(completion));
				{
					std::lock_guard<std::mutex> metrics_lock(metrics_mutex);
					metrics.mesh_worker_queued_completions =
						completions.size();
				}
			}
			std::function<void()> callback;
			{
				std::lock_guard<std::mutex> lock(notifier_mutex);
				callback = notifier;
			}
			if (callback) callback();
		}
	}

	std::size_t worker_count = 0;
	std::size_t queue_capacity = 0;
	std::size_t completion_capacity = 0;
	std::vector<std::thread> workers;
	mutable std::mutex work_mutex;
	std::condition_variable work_available;
	std::vector<PreparedMeshJob> work;
	mutable std::mutex completion_mutex;
	std::condition_variable completion_space;
	std::vector<PreparedMeshCompletion> completions;
	mutable std::mutex notifier_mutex;
	std::function<void()> notifier;
	mutable std::mutex metrics_mutex;
	WtPageMeshingRuntimeMetrics metrics;
	std::atomic<bool> stopping{ false };
};

WtPageMeshingRuntimeService::WtPageMeshingRuntimeService(
	std::size_t record_capacity,
	std::size_t meshing_worker_count
) :
		record_capacity_(record_capacity),
		valid_(record_capacity > 0 &&
			record_capacity <= kWtMaximumPageMeshingRuntimeRecords &&
			meshing_worker_count <= 8) {
	if (valid_) {
		records_.reserve(record_capacity_);
		loading_retry_candidates_.reserve(std::min(
			record_capacity_,
			std::size_t { 4 }
		));
		if (meshing_worker_count != 0) {
			async_ = std::make_unique<AsyncState>(
				meshing_worker_count,
				record_capacity
			);
		}
	}
}

WtPageMeshingRuntimeService::~WtPageMeshingRuntimeService() = default;

bool WtPageMeshingRuntimeService::valid() const noexcept {
	return valid_;
}

bool WtPageMeshingRuntimeService::asynchronous_meshing_enabled() const noexcept {
	return static_cast<bool>(async_);
}

bool WtPageMeshingRuntimeService::asynchronous_mesh_admission_available()
	const noexcept {
	return async_ != nullptr && async_->admission_available();
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::begin_sample_job(
	const WtChunkJob &job,
	std::uint8_t transition_mask,
	WtAsyncStorageService &storage,
	WtStoragePageCache &cache,
	WtStreamScheduler &scheduler
) {
	return begin_sample_job(
		job,
		transition_mask,
		transition_mask,
		storage,
		cache,
		scheduler
	);
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::begin_sample_job(
	const WtChunkJob &job,
	std::uint8_t transition_mask,
	std::uint8_t requested_cached_transition_mask,
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
		(requested_cached_transition_mask & 0xc0U) != 0 ||
		((transition_mask != 0 || requested_cached_transition_mask != 0) &&
			job.key.lod == 0)) {
		return WtPageMeshingRuntimeStatus::InvalidTransitionMask;
	}
	const std::uint8_t cached_transition_mask =
		supported_cached_transition_mask(
			job.key,
			transition_mask,
			requested_cached_transition_mask,
			storage
		);
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
		if ((cached_transition_mask & (1U << face_index)) == 0) {
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
	record.cached_transition_mask = cached_transition_mask;
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
WtPageMeshingRuntimeService::execute_mesh_job(
	const WtChunkJob &job,
	const WtChunkMesher &mesher,
	WtChunkMeshingScratch &scratch,
	WtStreamScheduler &scheduler,
	const WtEditJournal *edit_journal,
	std::uint64_t initial_world_revision,
	WtAsyncStorageService *authoritative_storage,
	const WtTerrainMeshReadyCallback &terrain_mesh_ready,
	bool visual_required
) {
	PreparedMeshJob prepared;
	const WtPageMeshingRuntimeStatus prepare_status = prepare_mesh_job(
		job,
		scheduler,
		edit_journal,
		initial_world_revision,
		authoritative_storage,
		terrain_mesh_ready,
		visual_required,
		{},
		prepared
	);
	if (prepare_status != WtPageMeshingRuntimeStatus::Ok) {
		return prepare_status;
	}
	PreparedMeshCompletion completion = execute_prepared_mesh_job(
		std::move(prepared), mesher, scratch
	);
	auto record = find_record(job.key);
	if (record != records_.end() && record->generation == job.generation &&
		record->phase == WtPageMeshingRuntimePhase::Meshing) {
		record->phase = WtPageMeshingRuntimePhase::AwaitingMesh;
	}
	return accept_prepared_mesh_completion(
		std::move(completion), scheduler, true
	);
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::dispatch_mesh_job(
	const WtChunkJob &job,
	WtStreamScheduler &scheduler,
	const WtEditJournal *edit_journal,
	std::uint64_t initial_world_revision,
	WtAsyncStorageService *authoritative_storage,
	const WtTerrainMeshReadyCallback &terrain_mesh_ready,
	bool visual_required,
	const WtMeshExecutionCallback &execution_callback
) {
	if (!async_) return WtPageMeshingRuntimeStatus::InvalidConfiguration;
	PreparedMeshJob prepared;
	const WtPageMeshingRuntimeStatus status = prepare_mesh_job(
		job,
		scheduler,
		edit_journal,
		initial_world_revision,
		authoritative_storage,
		terrain_mesh_ready,
		visual_required,
		execution_callback,
		prepared
	);
	if (status != WtPageMeshingRuntimeStatus::Ok) return status;
	if (async_->submit(std::move(prepared))) {
		return WtPageMeshingRuntimeStatus::Ok;
	}
	auto record = find_record(job.key);
	if (record != records_.end() && record->generation == job.generation &&
		record->phase == WtPageMeshingRuntimePhase::Meshing) {
		const std::size_t record_index = static_cast<std::size_t>(
			record - records_.begin()
		);
		for (Dependency &dependency : record->dependencies) {
			dependency.page.reset();
		}
		record->phase = WtPageMeshingRuntimePhase::MeshFailedReady;
		++metrics_.mesh_failures;
		++metrics_.scheduler_backpressure;
		record_failure_key(metrics_, record->key);
		submit_pending_result(record_index, scheduler);
	}
	return WtPageMeshingRuntimeStatus::SchedulerBackpressure;
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::process_async_mesh_completions(
	WtStreamScheduler &scheduler,
	std::size_t maximum_count,
	std::size_t &processed
) {
	processed = 0;
	if (!async_) return WtPageMeshingRuntimeStatus::InvalidConfiguration;
	PreparedMeshCompletion completion;
	while (processed < maximum_count && async_->pop_completion(completion)) {
		++processed;
		const WtPageMeshingRuntimeStatus status =
			accept_prepared_mesh_completion(std::move(completion), scheduler);
		if (status != WtPageMeshingRuntimeStatus::Ok &&
			status != WtPageMeshingRuntimeStatus::StaleCompletion &&
			status != WtPageMeshingRuntimeStatus::MeshingFailure &&
			status != WtPageMeshingRuntimeStatus::SchedulerBackpressure) {
			return status;
		}
	}
	return WtPageMeshingRuntimeStatus::Ok;
}

void WtPageMeshingRuntimeService::set_mesh_completion_notifier(
	std::function<void()> notifier
) {
	if (async_) async_->set_notifier(std::move(notifier));
}

void WtPageMeshingRuntimeService::merge_async_metrics(
	WtPageMeshingRuntimeMetrics &snapshot
) const noexcept {
	if (!async_) return;
	const WtPageMeshingRuntimeMetrics asynchronous = async_->snapshot();
	snapshot.mesh_worker_count = asynchronous.mesh_worker_count;
	snapshot.mesh_worker_accepted_jobs = asynchronous.mesh_worker_accepted_jobs;
	snapshot.mesh_worker_started_jobs = asynchronous.mesh_worker_started_jobs;
	snapshot.mesh_worker_completed_jobs = asynchronous.mesh_worker_completed_jobs;
	snapshot.mesh_worker_queue_rejections =
		asynchronous.mesh_worker_queue_rejections;
	snapshot.mesh_worker_cancelled_queued_jobs =
		asynchronous.mesh_worker_cancelled_queued_jobs;
	snapshot.mesh_worker_reprioritized_queued_jobs =
		asynchronous.mesh_worker_reprioritized_queued_jobs;
	snapshot.mesh_worker_queued_jobs = asynchronous.mesh_worker_queued_jobs;
	snapshot.mesh_worker_queued_completions =
		asynchronous.mesh_worker_queued_completions;
	snapshot.mesh_worker_active_jobs = asynchronous.mesh_worker_active_jobs;
	snapshot.mesh_worker_maximum_active_jobs =
		asynchronous.mesh_worker_maximum_active_jobs;
	snapshot.mesh_worker_queue_wait_ns_last =
		asynchronous.mesh_worker_queue_wait_ns_last;
	snapshot.mesh_worker_queue_wait_ns_total =
		asynchronous.mesh_worker_queue_wait_ns_total;
	snapshot.mesh_worker_queue_wait_ns_maximum =
		asynchronous.mesh_worker_queue_wait_ns_maximum;
	snapshot.mesh_worker_execute_time_ns_last =
		asynchronous.mesh_worker_execute_time_ns_last;
	snapshot.mesh_worker_execute_time_ns_total =
		asynchronous.mesh_worker_execute_time_ns_total;
	snapshot.mesh_worker_execute_time_ns_maximum =
		asynchronous.mesh_worker_execute_time_ns_maximum;
}

std::size_t WtPageMeshingRuntimeService::cancel_async_work(
	const WtChunkKey &key,
	WtGenerationToken generation
) noexcept {
	return async_ ? async_->cancel(key, generation) : 0;
}

bool WtPageMeshingRuntimeService::reprioritize_async_work(
	const WtChunkKey &key,
	WtGenerationToken generation,
	std::int32_t priority
) noexcept {
	return async_ && async_->reprioritize(key, generation, priority);
}

} // namespace world_transvoxel
