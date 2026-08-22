#include "streaming/wt_stream_scheduler.h"

#include <algorithm>
#include <utility>

namespace world_transvoxel {
namespace {

bool job_precedes(const WtChunkJob &a, const WtChunkJob &b) noexcept {
	if (a.priority != b.priority) {
		return a.priority > b.priority;
	}
	return a.sequence < b.sequence;
}

} // namespace

WtStreamScheduler::JobQueue::JobQueue(std::size_t capacity) : capacity_(capacity) {
	jobs_.reserve(capacity);
}

bool WtStreamScheduler::JobQueue::push(
	const WtChunkJob &job,
	WtSchedulerQueueTraceEvent *trace_event
) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (jobs_.size() >= capacity_) {
		return false;
	}
	const auto position = std::lower_bound(jobs_.begin(), jobs_.end(), job, job_precedes);
	if (trace_event != nullptr) {
		trace_event->kind = WtSchedulerQueueTraceEventKind::Queued;
		trace_event->job = job;
		trace_event->queue_depth_before = jobs_.size();
		trace_event->queue_depth_after = jobs_.size() + 1U;
		trace_event->jobs_ahead = static_cast<std::size_t>(
			position - jobs_.begin()
		);
		trace_event->same_priority_jobs_ahead =
			static_cast<std::size_t>(std::count_if(
				jobs_.begin(),
				position,
				[&job](const WtChunkJob &candidate) {
					return candidate.priority == job.priority;
				}
			));
	}
	jobs_.insert(position, job);
	return true;
}

bool WtStreamScheduler::JobQueue::pop(
	WtChunkJob &job,
	WtSchedulerQueueTraceEvent *trace_event
) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (jobs_.empty()) {
		return false;
	}
	job = jobs_.front();
	if (trace_event != nullptr) {
		trace_event->kind = WtSchedulerQueueTraceEventKind::Dequeued;
		trace_event->job = job;
		trace_event->queue_depth_before = jobs_.size();
		trace_event->queue_depth_after = jobs_.size() - 1U;
		trace_event->jobs_ahead = 0;
		trace_event->same_priority_jobs_ahead = 0;
	}
	jobs_.erase(jobs_.begin());
	return true;
}

bool WtStreamScheduler::JobQueue::reprioritize(
	const WtChunkKey &key,
	WtGenerationToken generation,
	std::int32_t priority,
	WtSchedulerQueueTraceEvent *trace_event
) {
	std::lock_guard<std::mutex> lock(mutex_);
	bool changed = false;
	for (WtChunkJob &job : jobs_) {
		if (job.key == key && job.generation == generation) {
			job.priority = priority;
			changed = true;
		}
	}
	if (changed) {
		std::sort(jobs_.begin(), jobs_.end(), job_precedes);
		if (trace_event != nullptr) {
			const auto found = std::find_if(
				jobs_.begin(),
				jobs_.end(),
				[&](const WtChunkJob &job) {
					return job.key == key && job.generation == generation;
				}
			);
			trace_event->kind =
				WtSchedulerQueueTraceEventKind::PriorityObserved;
			trace_event->job = *found;
			trace_event->queue_depth_before = jobs_.size();
			trace_event->queue_depth_after = jobs_.size();
			trace_event->jobs_ahead = static_cast<std::size_t>(
				found - jobs_.begin()
			);
			trace_event->same_priority_jobs_ahead =
				static_cast<std::size_t>(std::count_if(
					jobs_.begin(),
					found,
					[priority](const WtChunkJob &candidate) {
						return candidate.priority == priority;
					}
				));
		}
	}
	return changed;
}

bool WtStreamScheduler::JobQueue::observe(
	const WtChunkKey &key,
	WtGenerationToken generation,
	WtSchedulerQueueTraceEventKind kind,
	WtSchedulerQueueTraceEvent &trace_event
) const {
	std::lock_guard<std::mutex> lock(mutex_);
	const auto found = std::find_if(
		jobs_.begin(),
		jobs_.end(),
		[&](const WtChunkJob &job) {
			return job.key == key && job.generation == generation;
		}
	);
	if (found == jobs_.end()) {
		return false;
	}
	trace_event.kind = kind;
	trace_event.job = *found;
	trace_event.queue_depth_before = jobs_.size();
	trace_event.queue_depth_after = jobs_.size();
	trace_event.jobs_ahead = static_cast<std::size_t>(found - jobs_.begin());
	trace_event.same_priority_jobs_ahead =
		static_cast<std::size_t>(std::count_if(
			jobs_.begin(),
			found,
			[&](const WtChunkJob &candidate) {
				return candidate.priority == found->priority;
			}
		));
	return true;
}

std::size_t WtStreamScheduler::JobQueue::erase_key(const WtChunkKey &key) {
	std::lock_guard<std::mutex> lock(mutex_);
	const std::size_t previous_size = jobs_.size();
	jobs_.erase(
		std::remove_if(
			jobs_.begin(),
			jobs_.end(),
			[&key](const WtChunkJob &job) {
				return job.key == key;
			}
		),
		jobs_.end()
	);
	return previous_size - jobs_.size();
}

std::size_t WtStreamScheduler::JobQueue::size() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	return jobs_.size();
}

std::size_t WtStreamScheduler::JobQueue::available() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	return capacity_ - jobs_.size();
}

WtStreamScheduler::CompletionQueue::CompletionQueue(std::size_t capacity) :
		capacity_(capacity), slots_(capacity) {
}

bool WtStreamScheduler::CompletionQueue::push(const WtChunkJobResult &result) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (count_ >= capacity_ || capacity_ == 0) {
		return false;
	}
	const std::size_t tail = (head_ + count_) % capacity_;
	slots_[tail] = result;
	++count_;
	return true;
}

bool WtStreamScheduler::CompletionQueue::pop(WtChunkJobResult &result) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (count_ == 0) {
		return false;
	}
	result = slots_[head_];
	head_ = (head_ + 1) % capacity_;
	--count_;
	return true;
}

std::size_t WtStreamScheduler::CompletionQueue::erase_key(
	const WtChunkKey &key
) {
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<WtChunkJobResult> retained;
	retained.reserve(count_);
	for (std::size_t index = 0; index < count_; ++index) {
		const WtChunkJobResult &result =
			slots_[(head_ + index) % capacity_];
		if (result.key != key) {
			retained.push_back(result);
		}
	}
	const std::size_t erased = count_ - retained.size();
	for (std::size_t index = 0; index < retained.size(); ++index) {
		slots_[index] = retained[index];
	}
	head_ = 0;
	count_ = retained.size();
	return erased;
}

std::size_t WtStreamScheduler::CompletionQueue::size() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	return count_;
}

WtStreamScheduler::WtStreamScheduler(
	std::size_t record_capacity,
	std::size_t job_capacity,
	std::size_t completion_capacity,
	std::size_t viewer_capacity
) :
		record_capacity_(record_capacity),
		viewer_capacity_(viewer_capacity),
		jobs_(job_capacity),
		completions_(completion_capacity) {
	records_.reserve(record_capacity);
	viewers_.reserve(viewer_capacity);
}

WtSchedulerStatus WtStreamScheduler::request_chunk(
	const WtChunkKey &key,
	std::uint64_t source_revision,
	std::int32_t priority
) {
	return request_chunk_version(key, source_revision, 0, priority);
}

WtSchedulerStatus WtStreamScheduler::request_chunk_version(
	const WtChunkKey &key,
	std::uint64_t source_revision,
	std::uint64_t world_revision,
	std::int32_t priority,
	bool force_remesh
) {
	if (!wt_is_valid_chunk_key(key)) {
		return WtSchedulerStatus::InvalidKey;
	}
	WtChunkRecord *record = find_record_mutable(key);
	if (!force_remesh &&
		record != nullptr &&
		record->source_revision == source_revision &&
		record->world_revision == world_revision &&
		record->lifecycle == WtChunkLifecycle::Ready) {
		return WtSchedulerStatus::AlreadyCurrent;
	}
	if (record == nullptr && records_.size() >= record_capacity_) {
		return WtSchedulerStatus::RecordCapacityExceeded;
	}

	WtChunkRecord candidate = record != nullptr ? *record : WtChunkRecord{};
	candidate.key = key;
	candidate.generation = next_generation();
	candidate.source_revision = source_revision;
	candidate.world_revision = world_revision;
	candidate.priority = priority;
	candidate.lifecycle = WtChunkLifecycle::Sampling;
	const WtChunkJob job = make_job(candidate, WtChunkJobStage::Sample);
	const bool trace_enabled = queue_trace_enabled_.load(
		std::memory_order_acquire
	);
	WtSchedulerQueueTraceEvent trace_event;
	if (!jobs_.push(
			job,
			trace_enabled ? &trace_event : nullptr
		)) {
		++metrics_.queue_rejections;
		return WtSchedulerStatus::JobQueueFull;
	}
	if (trace_enabled) {
		notify_queue_trace(trace_event);
	}

	if (record == nullptr) {
		records_.push_back(candidate);
		std::sort(records_.begin(), records_.end(), [](const auto &a, const auto &b) {
			return a.key < b.key;
		});
	} else {
		*record = candidate;
	}
	++metrics_.requested_jobs;
	return WtSchedulerStatus::Ok;
}

WtSchedulerStatus WtStreamScheduler::cancel_chunk(const WtChunkKey &key) {
	WtChunkRecord *record = find_record_mutable(key);
	if (record == nullptr) {
		return WtSchedulerStatus::NotFound;
	}
	record->generation = next_generation();
	record->lifecycle = WtChunkLifecycle::Cancelled;
	++metrics_.cancellations;
	return WtSchedulerStatus::Ok;
}

WtSchedulerStatus WtStreamScheduler::forget_chunk(const WtChunkKey &key) {
	const auto iterator = std::lower_bound(
		records_.begin(),
		records_.end(),
		key,
		[](const WtChunkRecord &record, const WtChunkKey &value) {
			return record.key < value;
		}
	);
	if (iterator == records_.end() || iterator->key != key) {
		return WtSchedulerStatus::NotFound;
	}
	metrics_.discarded_jobs += jobs_.erase_key(key);
	metrics_.discarded_completions += completions_.erase_key(key);
	records_.erase(iterator);
	++metrics_.cancellations;
	return WtSchedulerStatus::Ok;
}

WtSchedulerStatus WtStreamScheduler::reprioritize_chunk(
	const WtChunkKey &key,
	std::int32_t priority
) {
	WtChunkRecord *record = find_record_mutable(key);
	if (record == nullptr) {
		return WtSchedulerStatus::NotFound;
	}
	if (record->priority == priority) {
		WtSchedulerQueueTraceEvent trace_event;
		if (queue_trace_enabled_.load(std::memory_order_acquire) && jobs_.observe(
				record->key,
				record->generation,
				WtSchedulerQueueTraceEventKind::PriorityObserved,
				trace_event
			)) {
			notify_queue_trace(trace_event);
		}
		return WtSchedulerStatus::AlreadyCurrent;
	}
	record->priority = priority;
	const bool trace_enabled = queue_trace_enabled_.load(
		std::memory_order_acquire
	);
	WtSchedulerQueueTraceEvent trace_event;
	const bool traced = jobs_.reprioritize(
		key,
		record->generation,
		priority,
		trace_enabled ? &trace_event : nullptr
	);
	if (trace_enabled && traced) {
		notify_queue_trace(trace_event);
	}
	return WtSchedulerStatus::Ok;
}

bool WtStreamScheduler::pop_job(WtChunkJob &job) {
	const bool trace_enabled = queue_trace_enabled_.load(
		std::memory_order_acquire
	);
	WtSchedulerQueueTraceEvent trace_event;
	const bool popped = jobs_.pop(
		job,
		trace_enabled ? &trace_event : nullptr
	);
	if (trace_enabled && popped) {
		notify_queue_trace(trace_event);
	}
	return popped;
}

WtSchedulerStatus WtStreamScheduler::submit_completion(const WtChunkJobResult &result) {
	if (!completions_.push(result)) {
		asynchronous_queue_rejections_.fetch_add(1, std::memory_order_relaxed);
		return WtSchedulerStatus::CompletionQueueFull;
	}
	return WtSchedulerStatus::Ok;
}

std::size_t WtStreamScheduler::apply_completions(std::size_t maximum_count) {
	std::size_t applied_count = 0;
	WtChunkJobResult result;
	while (applied_count < maximum_count && completions_.pop(result)) {
		++applied_count;
		WtChunkRecord *record = find_record_mutable(result.key);
		if (record == nullptr || record->generation != result.generation) {
			++metrics_.stale_results;
			continue;
		}
		const bool expected_stage =
			(result.stage == WtChunkJobStage::Sample &&
				record->lifecycle == WtChunkLifecycle::Sampling) ||
			(result.stage == WtChunkJobStage::Mesh &&
				record->lifecycle == WtChunkLifecycle::Meshing);
		if (!expected_stage) {
			++metrics_.stale_results;
			continue;
		}
		++metrics_.completed_jobs;
		if (!result.success) {
			record->lifecycle = WtChunkLifecycle::Failed;
			continue;
		}
		if (result.stage == WtChunkJobStage::Sample) {
			const WtChunkJob mesh_job = make_job(*record, WtChunkJobStage::Mesh);
			const bool trace_enabled = queue_trace_enabled_.load(
				std::memory_order_acquire
			);
			WtSchedulerQueueTraceEvent trace_event;
			if (!jobs_.push(
					mesh_job,
					trace_enabled ? &trace_event : nullptr
				)) {
				++metrics_.queue_rejections;
				record->lifecycle = WtChunkLifecycle::Failed;
				continue;
			}
			if (trace_enabled) {
				notify_queue_trace(trace_event);
			}
			record->lifecycle = WtChunkLifecycle::Meshing;
			++metrics_.requested_jobs;
		} else {
			record->lifecycle = WtChunkLifecycle::Ready;
		}
	}
	return applied_count;
}

WtSchedulerStatus WtStreamScheduler::update_viewer(const WtViewerSnapshot &snapshot) {
	const auto iterator = std::lower_bound(
		viewers_.begin(),
		viewers_.end(),
		snapshot.id,
		[](const WtViewerSnapshot &viewer, std::uint64_t id) {
			return viewer.id < id;
		}
	);
	if (iterator != viewers_.end() && iterator->id == snapshot.id) {
		if (snapshot.revision <= iterator->revision) {
			return WtSchedulerStatus::StaleViewerSnapshot;
		}
		*iterator = snapshot;
		return WtSchedulerStatus::Ok;
	}
	if (viewers_.size() >= viewer_capacity_) {
		return WtSchedulerStatus::ViewerCapacityExceeded;
	}
	viewers_.insert(iterator, snapshot);
	return WtSchedulerStatus::Ok;
}

const WtChunkRecord *WtStreamScheduler::find_record(const WtChunkKey &key) const noexcept {
	const auto iterator = std::lower_bound(
		records_.begin(),
		records_.end(),
		key,
		[](const WtChunkRecord &record, const WtChunkKey &value) {
			return record.key < value;
		}
	);
	return iterator != records_.end() && iterator->key == key ? &*iterator : nullptr;
}

WtChunkRecord *WtStreamScheduler::find_record_mutable(const WtChunkKey &key) noexcept {
	return const_cast<WtChunkRecord *>(
		static_cast<const WtStreamScheduler *>(this)->find_record(key)
	);
}

const std::vector<WtChunkRecord> &WtStreamScheduler::get_records() const noexcept {
	return records_;
}

const std::vector<WtViewerSnapshot> &WtStreamScheduler::get_viewers() const noexcept {
	return viewers_;
}

WtSchedulerMetrics WtStreamScheduler::get_metrics() const noexcept {
	WtSchedulerMetrics snapshot = metrics_;
	snapshot.queue_rejections +=
		asynchronous_queue_rejections_.load(std::memory_order_relaxed);
	for (const WtChunkRecord &record : records_) {
		switch (record.lifecycle) {
			case WtChunkLifecycle::Requested:
				++snapshot.requested_records;
				break;
			case WtChunkLifecycle::Sampling:
				++snapshot.sampling_records;
				break;
			case WtChunkLifecycle::Meshing:
				++snapshot.meshing_records;
				break;
			case WtChunkLifecycle::Ready:
				++snapshot.ready_records;
				break;
			case WtChunkLifecycle::Failed:
				++snapshot.failed_records;
				break;
			case WtChunkLifecycle::Cancelled:
				break;
		}
	}
	return snapshot;
}

std::size_t WtStreamScheduler::record_capacity() const noexcept {
	return record_capacity_;
}

std::size_t WtStreamScheduler::available_record_capacity() const noexcept {
	return record_capacity_ - records_.size();
}

std::size_t WtStreamScheduler::queued_job_count() const noexcept {
	return jobs_.size();
}

std::size_t WtStreamScheduler::available_job_capacity() const noexcept {
	return jobs_.available();
}

std::size_t WtStreamScheduler::queued_completion_count() const noexcept {
	return completions_.size();
}

void WtStreamScheduler::set_queue_trace_observer(
	WtSchedulerQueueTraceObserver observer
) {
	const bool enabled = static_cast<bool>(observer);
	std::lock_guard<std::mutex> lock(queue_trace_observer_mutex_);
	queue_trace_observer_ = std::move(observer);
	queue_trace_enabled_.store(enabled, std::memory_order_release);
}

void WtStreamScheduler::notify_queue_trace(
	const WtSchedulerQueueTraceEvent &event
) {
	WtSchedulerQueueTraceObserver observer;
	{
		std::lock_guard<std::mutex> lock(queue_trace_observer_mutex_);
		observer = queue_trace_observer_;
	}
	if (observer) {
		observer(event);
	}
}

WtGenerationToken WtStreamScheduler::next_generation() noexcept {
	++generation_counter_;
	if (generation_counter_ == 0) {
		++generation_counter_;
	}
	return { generation_counter_ };
}

WtChunkJob WtStreamScheduler::make_job(
	const WtChunkRecord &record,
	WtChunkJobStage stage
) noexcept {
	return {
		record.key,
		record.generation,
		record.source_revision,
		record.world_revision,
		++sequence_counter_,
		record.priority,
		stage,
	};
}

} // namespace world_transvoxel
