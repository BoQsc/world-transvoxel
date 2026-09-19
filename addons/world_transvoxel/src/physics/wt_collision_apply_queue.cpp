#include "physics/wt_collision_apply_queue.h"

#include <algorithm>

namespace world_transvoxel {

WtCollisionApplyQueue::WtCollisionApplyQueue(std::size_t capacity) : capacity_(capacity) {
	queue_.reserve(capacity);
}

WtApplicationStatus WtCollisionApplyQueue::submit(
	const WtCollisionPayloadPtr &payload,
	std::uint64_t submission_tick,
	bool interaction_critical
) {
	if (!payload || !wt_is_valid_chunk_key(payload->key) || payload->generation.value == 0) {
		return WtApplicationStatus::InvalidInput;
	}
	const auto duplicate = std::find_if(
		queue_.begin(), queue_.end(),
		[&payload](const WtCollisionApplyEntry &entry) {
			return entry.payload && entry.payload->key == payload->key &&
				entry.payload->generation == payload->generation;
		}
	);
	if (duplicate != queue_.end()) {
		duplicate->payload = payload;
		duplicate->interaction_critical =
			duplicate->interaction_critical || interaction_critical;
		return WtApplicationStatus::Ok;
	}
	if (queue_.size() >= capacity_) return WtApplicationStatus::QueueFull;
	queue_.push_back({ payload, submission_tick, interaction_critical });
	return WtApplicationStatus::Ok;
}

bool WtCollisionApplyQueue::pop(WtCollisionApplyEntry &entry) {
	if (queue_.empty()) return false;
	auto selected = std::find_if(
		queue_.begin(), queue_.end(),
		[](const WtCollisionApplyEntry &candidate) {
			return candidate.interaction_critical;
		}
	);
	if (selected == queue_.end()) selected = queue_.begin();
	entry = std::move(*selected);
	queue_.erase(selected);
	return true;
}

std::size_t WtCollisionApplyQueue::size() const noexcept {
	return queue_.size();
}

std::size_t WtCollisionApplyQueue::capacity() const noexcept {
	return capacity_;
}

} // namespace world_transvoxel
