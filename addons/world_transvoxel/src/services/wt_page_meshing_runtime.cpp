#include "services/wt_page_meshing_runtime.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "bake/wt_chunk_baker.h"
#include "editing/wt_chunk_edit_state.h"
#include "meshing/wt_material_volume_sample_source.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_chunk_page_sample_source.h"
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

bool pending_result_phase(WtPageMeshingRuntimePhase phase) noexcept {
	return phase == WtPageMeshingRuntimePhase::SampleReady ||
		phase == WtPageMeshingRuntimePhase::SampleFailedReady ||
		phase == WtPageMeshingRuntimePhase::MeshReady ||
		phase == WtPageMeshingRuntimePhase::MeshFailedReady;
}

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

std::uint64_t steady_time_ns() noexcept {
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()
		).count()
	);
}

} // namespace

struct WtPageMeshingRuntimeService::PreparedDependency {
	WtChunkKey key;
	std::shared_ptr<const WtChunkPage> page;
};

struct WtPageMeshingRuntimeService::PreparedMeshJob {
	WtChunkJob job;
	std::uint8_t transition_mask = 0;
	std::uint8_t cached_transition_mask = 0;
	bool visual_required = true;
	std::vector<PreparedDependency> dependencies;
	WtTerrainMeshReadyCallback terrain_mesh_ready;
	WtMeshExecutionCallback execution_callback;
	std::uint64_t enqueued_time_ns = 0;
};

struct WtPageMeshingRuntimeService::PreparedMeshCompletion {
	PreparedMeshJob prepared;
	std::shared_ptr<WtChunkMeshResult> mesh;
	std::shared_ptr<WtChunkMeshResult> water_mesh;
	WtPageMeshingRuntimeStatus status =
		WtPageMeshingRuntimeStatus::MeshingFailure;
	std::uint64_t queue_wait_ns = 0;
	std::uint64_t execute_time_ns = 0;
};

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
			record_failure_key(metrics_, record->key);
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
	prepared.dependencies.reserve(record->dependencies.size());
	for (const Dependency &dependency : record->dependencies) {
		if (!dependency.page) source_valid = false;
		prepared.dependencies.push_back({ dependency.key, dependency.page });
	}
	if (!source_valid) {
		record->phase = WtPageMeshingRuntimePhase::MeshFailedReady;
		++metrics_.mesh_failures;
		record_failure_key(metrics_, record->key);
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
	const bool mesh_ok = source_valid && mesher.mesh(
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
	) == WtChunkMeshingStatus::Ok;
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
		water_mesh_ok = mesher.mesh(
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
		) == WtChunkMeshingStatus::Ok;
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
	for (Dependency &dependency : record->dependencies) {
		dependency.page.reset();
	}
	if (completion.status != WtPageMeshingRuntimeStatus::Ok ||
		!completion.mesh || !completion.water_mesh) {
		record->phase = WtPageMeshingRuntimePhase::MeshFailedReady;
		++metrics_.mesh_failures;
		record_failure_key(metrics_, record->key);
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
		record_failure_key(metrics_, dependency.key);
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
