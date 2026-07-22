#include "services/wt_chunk_application.h"

#include <algorithm>

namespace world_transvoxel {

bool WtChunkApplicationRecord::fully_ready() const noexcept {
	return (!visual_required || visual_ready) &&
		(!collision_required || collision_ready);
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
	bool staged_replacement
) {
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
			return WtApplicationStatus::AlreadyCurrent;
		}
		*record = {
			key,
			generation,
			collision_required,
			visual_required,
			false,
			false,
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
	}
	return WtApplicationStatus::Ok;
}

WtApplicationStatus WtChunkApplicationService::set_collision_required(
	const WtChunkKey &key,
	bool required
) {
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
	}
	return WtApplicationStatus::Ok;
}

WtApplicationStatus WtChunkApplicationService::forget_chunk(const WtChunkKey &key) {
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
	const std::uint64_t tick =
		application_tick_.fetch_add(1, std::memory_order_relaxed) + 1;
	return {
		apply_render(render_budget, render_sink, tick),
		apply_collision(collision_budget, collision_sink, tick),
	};
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
			++metrics_.stale_render;
			continue;
		}
		if (!record->visual_required) {
			++metrics_.stale_render;
			continue;
		}
		if (!sink.apply_render(*payload)) {
			++metrics_.sink_failures;
			continue;
		}
		record->visual_ready = true;
		if (record->fully_ready()) {
			record->staged_replacement = false;
		}
		++metrics_.applied_render;
	}
	return processed;
}

std::size_t WtChunkApplicationService::apply_deferred_collisions(
	std::size_t budget,
	WtCollisionSink &sink,
	std::uint64_t application_tick
) {
	std::size_t processed = 0;
	for (auto iterator = deferred_collisions_.begin();
			iterator != deferred_collisions_.end() && processed < budget;) {
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
		if (!sink.apply_collision(*payload)) {
			++metrics_.sink_failures;
			iterator = deferred_collisions_.erase(iterator);
			continue;
		}
		record->collision_ready = true;
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
	std::uint64_t application_tick
) {
	std::size_t processed =
		apply_deferred_collisions(budget, sink, application_tick);
	WtCollisionApplyEntry entry;
	while (processed < budget && collision_queue_.pop(entry)) {
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
		if (!sink.apply_collision(*payload)) {
			++metrics_.sink_failures;
			continue;
		}
		record->collision_ready = true;
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

WtChunkApplicationRecord *WtChunkApplicationService::find_record_mutable(
	const WtChunkKey &key
) noexcept {
	return const_cast<WtChunkApplicationRecord *>(
		static_cast<const WtChunkApplicationService *>(this)->find_record(key)
	);
}

const std::vector<WtChunkApplicationRecord> &
WtChunkApplicationService::get_records() const noexcept {
	return records_;
}

WtApplicationMetrics WtChunkApplicationService::get_metrics() const noexcept {
	WtApplicationMetrics snapshot = metrics_;
	snapshot.submitted_render =
		asynchronous_render_submissions_.load(std::memory_order_relaxed);
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
	return record_capacity_ - records_.size();
}

std::size_t WtChunkApplicationService::queued_render_count() const noexcept {
	return render_queue_.size();
}

std::size_t WtChunkApplicationService::queued_collision_count() const noexcept {
	return collision_queue_.size();
}

} // namespace world_transvoxel
