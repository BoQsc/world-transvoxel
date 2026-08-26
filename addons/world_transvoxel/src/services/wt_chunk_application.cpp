#include "services/wt_chunk_application.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace world_transvoxel {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(const Clock::time_point &start) noexcept {
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			Clock::now() - start
		).count()
	);
}

bool collision_deadline_reached(
	const Clock::time_point &start,
	std::uint64_t deadline_ns
) noexcept {
	return deadline_ns != 0 && elapsed_ns(start) >= deadline_ns;
}

} // namespace

bool WtChunkApplicationRecord::fully_ready() const noexcept {
	return (!visual_required ||
			(visual_ready && visual_generation == generation)) &&
		(!collision_required ||
			(collision_ready && collision_generation == generation));
}

WtChunkApplicationService::WtChunkApplicationService(
	std::size_t record_capacity,
	std::size_t render_queue_capacity,
	std::size_t collision_queue_capacity
) :
		record_capacity_(record_capacity),
		render_queue_(render_queue_capacity),
		collision_queue_(collision_queue_capacity) {
	records_.reserve(record_capacity);
	deferred_collisions_.reserve(collision_queue_capacity);
}

WtApplicationStatus WtChunkApplicationService::expect_chunk(
	const WtChunkKey &key,
	WtGenerationToken generation,
	bool collision_required,
	bool visual_required,
	bool staged_replacement,
	bool preserve_collision_ready
) {
	std::lock_guard<std::mutex> lock(records_mutex_);
	if (!wt_is_valid_chunk_key(key) || generation.value == 0 ||
		(!visual_required && !collision_required)) {
		return WtApplicationStatus::InvalidInput;
	}
	WtChunkApplicationRecord *record = find_record_mutable(key);
	if (record != nullptr) {
		if (generation.value < record->generation.value) {
			return WtApplicationStatus::StaleGeneration;
		}
		if (generation == record->generation) {
			bool changed = false;
			if (visual_required && !record->visual_required) {
				record->visual_required = true;
				record->visual_ready = false;
				record->external_visual_activation_required = false;
				changed = true;
			}
			if (collision_required && !record->collision_required) {
				record->collision_required = true;
				if (!(preserve_collision_ready && record->collision_ready)) {
					record->collision_ready = false;
				}
				changed = true;
			}
			if (staged_replacement && !record->staged_replacement) {
				record->staged_replacement = true;
				changed = true;
			}
			return changed ? WtApplicationStatus::Ok :
				WtApplicationStatus::AlreadyCurrent;
		}
		const bool carried_collision_ready =
			preserve_collision_ready && collision_required &&
			record->collision_required && record->collision_ready;
		const WtGenerationToken carried_collision_generation =
			carried_collision_ready ? record->collision_generation :
				WtGenerationToken{};
		*record = {
			key,
			generation,
			{},
			carried_collision_generation,
			collision_required,
			visual_required,
			false,
			carried_collision_ready,
			staged_replacement,
		};
		return WtApplicationStatus::Ok;
	}
	if (records_.size() >= record_capacity_) {
		return WtApplicationStatus::RecordCapacityExceeded;
	}
	records_.push_back({
		key,
		generation,
		{},
		{},
		collision_required,
		visual_required,
		false,
		false,
		staged_replacement,
	});
	std::sort(records_.begin(), records_.end(), [](const auto &a, const auto &b) {
		return a.key < b.key;
	});
	return WtApplicationStatus::Ok;
}

WtApplicationStatus WtChunkApplicationService::set_visual_required(
	const WtChunkKey &key,
	bool required
) {
	std::lock_guard<std::mutex> lock(records_mutex_);
	WtChunkApplicationRecord *record = find_record_mutable(key);
	if (record == nullptr) {
		return WtApplicationStatus::NotFound;
	}
	if (!required && !record->collision_required) {
		return WtApplicationStatus::InvalidInput;
	}
	if (record->visual_required == required) {
		return WtApplicationStatus::AlreadyCurrent;
	}
	record->visual_required = required;
	if (required) {
		record->visual_ready = false;
		record->visual_generation = {};
		record->external_visual_activation_required = false;
	}
	return WtApplicationStatus::Ok;
}

WtApplicationStatus WtChunkApplicationService::set_collision_required(
	const WtChunkKey &key,
	bool required
) {
	std::lock_guard<std::mutex> lock(records_mutex_);
	WtChunkApplicationRecord *record = find_record_mutable(key);
	if (record == nullptr) {
		return WtApplicationStatus::NotFound;
	}
	if (record->collision_required == required) {
		return WtApplicationStatus::AlreadyCurrent;
	}
	if (!required && !record->visual_required) {
		return WtApplicationStatus::InvalidInput;
	}
	record->collision_required = required;
	if (required) {
		record->collision_ready = false;
		record->collision_generation = {};
	}
	return WtApplicationStatus::Ok;
}

WtApplicationStatus WtChunkApplicationService::confirm_external_visual_activation(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	std::lock_guard<std::mutex> lock(records_mutex_);
	WtChunkApplicationRecord *record = find_record_mutable(key);
	if (record == nullptr) return WtApplicationStatus::NotFound;
	if (record->generation != generation ||
		record->visual_generation != generation) {
		return WtApplicationStatus::StaleGeneration;
	}
	if (!record->visual_required) return WtApplicationStatus::InvalidInput;
	if (!record->external_visual_activation_required) {
		return record->visual_ready ? WtApplicationStatus::AlreadyCurrent :
			WtApplicationStatus::InvalidInput;
	}
	record->external_visual_activation_required = false;
	record->visual_ready = true;
	if (record->fully_ready()) record->staged_replacement = false;
	return WtApplicationStatus::Ok;
}

WtApplicationStatus WtChunkApplicationService::forget_chunk(const WtChunkKey &key) {
	std::lock_guard<std::mutex> lock(records_mutex_);
	const auto iterator = std::lower_bound(
		records_.begin(), records_.end(), key,
		[](const WtChunkApplicationRecord &record, const WtChunkKey &value) {
			return record.key < value;
		}
	);
	if (iterator == records_.end() || iterator->key != key) {
		return WtApplicationStatus::NotFound;
	}
	records_.erase(iterator);
	return WtApplicationStatus::Ok;
}

WtApplicationStatus WtChunkApplicationService::submit_render(
	const WtRenderPayloadPtr &payload
) {
	const WtApplicationStatus status = render_queue_.submit(
		payload, application_tick_.load(std::memory_order_relaxed)
	);
	if (status == WtApplicationStatus::Ok) {
		asynchronous_render_submissions_.fetch_add(1, std::memory_order_relaxed);
		if (payload && payload->publication_source ==
				WtRenderPublicationSource::GpuCellCandidate) {
			asynchronous_gpu_candidate_render_submissions_.fetch_add(
				1, std::memory_order_relaxed
			);
		}
	} else if (status == WtApplicationStatus::QueueFull) {
		asynchronous_queue_rejections_.fetch_add(1, std::memory_order_relaxed);
	}
	return status;
}

WtApplicationStatus WtChunkApplicationService::submit_collision(
	const WtCollisionPayloadPtr &payload
) {
	const WtApplicationStatus status = collision_queue_.submit(
		payload, application_tick_.load(std::memory_order_relaxed)
	);
	if (status == WtApplicationStatus::Ok) {
		asynchronous_collision_submissions_.fetch_add(1, std::memory_order_relaxed);
	} else if (status == WtApplicationStatus::QueueFull) {
		asynchronous_queue_rejections_.fetch_add(1, std::memory_order_relaxed);
	}
	return status;
}

WtApplicationBatchResult WtChunkApplicationService::apply(
	std::size_t render_budget,
	std::size_t collision_budget,
	WtRenderSink &render_sink,
	WtCollisionSink &collision_sink
) {
	return apply_with_collision_deadline(
		render_budget,
		collision_budget,
		0,
		render_sink,
		collision_sink
	);
}

WtApplicationBatchResult
WtChunkApplicationService::apply_with_collision_deadline(
	std::size_t render_budget,
	std::size_t collision_budget,
	std::uint64_t collision_deadline_ns,
	WtRenderSink &render_sink,
	WtCollisionSink &collision_sink
) {
	std::lock_guard<std::mutex> lock(records_mutex_);
	const std::uint64_t tick =
		application_tick_.fetch_add(1, std::memory_order_relaxed) + 1;
	WtApplicationBatchResult result;
	result.render_processed = apply_render(render_budget, render_sink, tick);
	const Clock::time_point collision_start = Clock::now();
	result.collision_processed = apply_collision(
		collision_budget,
		collision_sink,
		tick,
		collision_start,
		collision_deadline_ns,
		result.collision_deadline_exhausted
	);
	const std::uint64_t collision_elapsed = elapsed_ns(collision_start);
	metrics_.collision_apply_time_ns_last = collision_elapsed;
	metrics_.collision_apply_time_ns_total += collision_elapsed;
	metrics_.collision_apply_time_ns_maximum = std::max(
		metrics_.collision_apply_time_ns_maximum,
		collision_elapsed
	);
	if (result.collision_deadline_exhausted) {
		++metrics_.collision_apply_deadline_exhaustions;
	}
	return result;
}

std::size_t WtChunkApplicationService::apply_render(
	std::size_t budget,
	WtRenderSink &sink,
	std::uint64_t application_tick
) {
	std::size_t processed = 0;
	WtRenderApplyEntry entry;
	while (processed < budget && render_queue_.pop(entry)) {
		++processed;
		const WtRenderPayloadPtr &payload = entry.payload;
		const std::uint64_t latency = application_tick - entry.submission_tick;
		metrics_.render_latency_frames_total += latency;
		metrics_.render_latency_frames_maximum = std::max(
			metrics_.render_latency_frames_maximum, latency
		);
		WtChunkApplicationRecord *record = find_record_mutable(payload->key);
		if (record == nullptr || record->generation != payload->generation) {
			metrics_.last_stale_render_key_x = payload->key.x;
			metrics_.last_stale_render_key_y = payload->key.y;
			metrics_.last_stale_render_key_z = payload->key.z;
			metrics_.last_stale_render_key_lod = payload->key.lod;
			metrics_.last_stale_render_generation =
				payload->generation.value;
			metrics_.last_stale_render_record_generation =
				record == nullptr ? 0U : record->generation.value;
			++metrics_.stale_render;
			if (payload->publication_source ==
					WtRenderPublicationSource::GpuCellCandidate) {
				++metrics_.stale_gpu_candidate_render;
			}
			continue;
		}
		if (!record->visual_required) {
			++metrics_.stale_render;
			if (payload->publication_source ==
					WtRenderPublicationSource::GpuCellCandidate) {
				++metrics_.stale_gpu_candidate_render;
			}
			continue;
		}
		const bool trace_enabled = static_cast<bool>(trace_observer_);
		const Clock::time_point sink_started = trace_enabled ?
			Clock::now() : Clock::time_point{};
		const bool sink_applied = sink.apply_render(*payload);
		if (trace_enabled) {
			trace_observer_(
				false,
				payload->key,
				payload->generation,
				elapsed_ns(sink_started),
				sink_applied
			);
		}
		if (!sink_applied) {
			++metrics_.sink_failures;
			continue;
		}
		record->visual_generation = payload->generation;
		record->external_visual_activation_required =
			payload->publication_source ==
				WtRenderPublicationSource::GpuResidentPlaceholder;
		record->visual_ready = !record->external_visual_activation_required;
		if (record->fully_ready()) {
			record->staged_replacement = false;
		}
		++metrics_.applied_render;
		if (payload->publication_source ==
				WtRenderPublicationSource::GpuCellCandidate) {
			++metrics_.applied_gpu_candidate_render;
		}
	}
	return processed;
}

std::size_t WtChunkApplicationService::apply_deferred_collisions(
	std::size_t budget,
	WtCollisionSink &sink,
	std::uint64_t application_tick,
	const Clock::time_point &deadline_start,
	std::uint64_t collision_deadline_ns,
	bool &deadline_exhausted
) {
	std::size_t processed = 0;
	for (auto iterator = deferred_collisions_.begin();
			iterator != deferred_collisions_.end() && processed < budget;) {
		if (processed > 0 &&
			collision_deadline_reached(
				deadline_start, collision_deadline_ns
			)) {
			deadline_exhausted = true;
			break;
		}
		const WtCollisionApplyEntry entry = *iterator;
		const WtCollisionPayloadPtr &payload = entry.payload;
		WtChunkApplicationRecord *record = find_record_mutable(payload->key);
		if (record == nullptr || record->generation != payload->generation) {
			++processed;
			const std::uint64_t latency =
				application_tick - entry.submission_tick;
			metrics_.collision_latency_frames_total += latency;
			metrics_.collision_latency_frames_maximum = std::max(
				metrics_.collision_latency_frames_maximum, latency
			);
			++metrics_.stale_collision;
			iterator = deferred_collisions_.erase(iterator);
			continue;
		}
		if (!record->collision_required) {
			++processed;
			const std::uint64_t latency =
				application_tick - entry.submission_tick;
			metrics_.collision_latency_frames_total += latency;
			metrics_.collision_latency_frames_maximum = std::max(
				metrics_.collision_latency_frames_maximum, latency
			);
			++metrics_.unrequired_collision;
			iterator = deferred_collisions_.erase(iterator);
			continue;
		}
		if (should_defer_collision(*record)) {
			++iterator;
			continue;
		}
		++processed;
		const std::uint64_t latency = application_tick - entry.submission_tick;
		metrics_.collision_latency_frames_total += latency;
		metrics_.collision_latency_frames_maximum = std::max(
			metrics_.collision_latency_frames_maximum, latency
		);
		const bool trace_enabled = static_cast<bool>(trace_observer_);
		const Clock::time_point sink_started = trace_enabled ?
			Clock::now() : Clock::time_point{};
		const bool sink_applied = sink.apply_collision(*payload);
		if (trace_enabled) {
			trace_observer_(
				true,
				payload->key,
				payload->generation,
				elapsed_ns(sink_started),
				sink_applied
			);
		}
		if (!sink_applied) {
			++metrics_.sink_failures;
			iterator = deferred_collisions_.erase(iterator);
			continue;
		}
		record->collision_ready = true;
		record->collision_generation = payload->generation;
		if (record->fully_ready()) {
			record->staged_replacement = false;
		}
		++metrics_.applied_collision;
		iterator = deferred_collisions_.erase(iterator);
	}
	return processed;
}

std::size_t WtChunkApplicationService::apply_collision(
	std::size_t budget,
	WtCollisionSink &sink,
	std::uint64_t application_tick,
	const Clock::time_point &deadline_start,
	std::uint64_t collision_deadline_ns,
	bool &deadline_exhausted
) {
	std::size_t processed =
		apply_deferred_collisions(
			budget,
			sink,
			application_tick,
			deadline_start,
			collision_deadline_ns,
			deadline_exhausted
		);
	WtCollisionApplyEntry entry;
	while (processed < budget && collision_queue_.pop(entry)) {
		if (processed > 0 &&
			collision_deadline_reached(
				deadline_start, collision_deadline_ns
			)) {
			if (!defer_collision(entry)) {
				++metrics_.queue_rejections;
			}
			deadline_exhausted = true;
			break;
		}
		++processed;
		const WtCollisionPayloadPtr &payload = entry.payload;
		const std::uint64_t latency = application_tick - entry.submission_tick;
		WtChunkApplicationRecord *record = find_record_mutable(payload->key);
		if (record == nullptr || record->generation != payload->generation) {
			metrics_.collision_latency_frames_total += latency;
			metrics_.collision_latency_frames_maximum = std::max(
				metrics_.collision_latency_frames_maximum, latency
			);
			++metrics_.stale_collision;
			continue;
		}
		if (!record->collision_required) {
			metrics_.collision_latency_frames_total += latency;
			metrics_.collision_latency_frames_maximum = std::max(
				metrics_.collision_latency_frames_maximum, latency
			);
			++metrics_.unrequired_collision;
			continue;
		}
		if (should_defer_collision(*record)) {
			if (!defer_collision(entry)) {
				++metrics_.queue_rejections;
			}
			continue;
		}
		metrics_.collision_latency_frames_total += latency;
		metrics_.collision_latency_frames_maximum = std::max(
			metrics_.collision_latency_frames_maximum, latency
		);
		const bool trace_enabled = static_cast<bool>(trace_observer_);
		const Clock::time_point sink_started = trace_enabled ?
			Clock::now() : Clock::time_point{};
		const bool sink_applied = sink.apply_collision(*payload);
		if (trace_enabled) {
			trace_observer_(
				true,
				payload->key,
				payload->generation,
				elapsed_ns(sink_started),
				sink_applied
			);
		}
		if (!sink_applied) {
			++metrics_.sink_failures;
			continue;
		}
		record->collision_ready = true;
		record->collision_generation = payload->generation;
		if (record->fully_ready()) {
			record->staged_replacement = false;
		}
		++metrics_.applied_collision;
	}
	return processed;
}

bool WtChunkApplicationService::should_defer_collision(
	const WtChunkApplicationRecord &record
) const noexcept {
	return record.staged_replacement && record.visual_required &&
		!record.visual_ready;
}

void WtChunkApplicationService::set_trace_observer(
	WtApplicationTraceObserver observer
) {
	std::lock_guard<std::mutex> lock(records_mutex_);
	trace_observer_ = std::move(observer);
}

bool WtChunkApplicationService::defer_collision(
	const WtCollisionApplyEntry &entry
) {
	if (!entry.payload) return false;
	for (WtCollisionApplyEntry &deferred : deferred_collisions_) {
		if (deferred.payload && deferred.payload->key == entry.payload->key) {
			if (deferred.payload->generation.value <=
				entry.payload->generation.value) {
				deferred = entry;
			}
			return true;
		}
	}
	if (deferred_collisions_.size() >= collision_queue_.capacity()) {
		return false;
	}
	deferred_collisions_.push_back(entry);
	return true;
}

const WtChunkApplicationRecord *WtChunkApplicationService::find_record(
	const WtChunkKey &key
) const noexcept {
	const auto iterator = std::lower_bound(
		records_.begin(), records_.end(), key,
		[](const WtChunkApplicationRecord &record, const WtChunkKey &value) {
			return record.key < value;
		}
	);
	return iterator != records_.end() && iterator->key == key ? &*iterator : nullptr;
}

bool WtChunkApplicationService::copy_record(
	const WtChunkKey &key,
	WtChunkApplicationRecord &record
) const noexcept {
	std::lock_guard<std::mutex> lock(records_mutex_);
	const WtChunkApplicationRecord *found = find_record(key);
	if (found == nullptr) return false;
	record = *found;
	return true;
}

WtChunkApplicationRecord *WtChunkApplicationService::find_record_mutable(
	const WtChunkKey &key
) noexcept {
	const auto iterator = std::lower_bound(
		records_.begin(), records_.end(), key,
		[](const WtChunkApplicationRecord &record, const WtChunkKey &value) {
			return record.key < value;
		}
	);
	return iterator != records_.end() && iterator->key == key ?
		&*iterator : nullptr;
}

std::vector<WtChunkApplicationRecord>
WtChunkApplicationService::get_records() const {
	std::lock_guard<std::mutex> lock(records_mutex_);
	return records_;
}

WtApplicationMetrics WtChunkApplicationService::get_metrics() const noexcept {
	WtApplicationMetrics snapshot = metrics_;
	snapshot.submitted_render =
		asynchronous_render_submissions_.load(std::memory_order_relaxed);
	snapshot.submitted_gpu_candidate_render =
		asynchronous_gpu_candidate_render_submissions_.load(
			std::memory_order_relaxed
		);
	snapshot.submitted_collision =
		asynchronous_collision_submissions_.load(std::memory_order_relaxed);
	snapshot.queue_rejections =
		asynchronous_queue_rejections_.load(std::memory_order_relaxed);
	return snapshot;
}

std::size_t WtChunkApplicationService::record_capacity() const noexcept {
	return record_capacity_;
}

std::size_t
WtChunkApplicationService::available_record_capacity() const noexcept {
	std::lock_guard<std::mutex> lock(records_mutex_);
	return record_capacity_ - records_.size();
}

std::size_t WtChunkApplicationService::queued_render_count() const noexcept {
	return render_queue_.size();
}

std::size_t WtChunkApplicationService::queued_collision_count() const noexcept {
	return collision_queue_.size();
}

std::size_t WtChunkApplicationService::deferred_collision_count() const noexcept {
	std::lock_guard<std::mutex> lock(records_mutex_);
	return deferred_collisions_.size();
}

} // namespace world_transvoxel
