#include "physics/wt_collision_apply_queue.h"

namespace world_transvoxel {

WtCollisionApplyQueue::WtCollisionApplyQueue(std::size_t capacity) : queue_(capacity) {
}

WtApplicationStatus WtCollisionApplyQueue::submit(
	const WtCollisionPayloadPtr &payload
) {
	if (!payload || !wt_is_valid_chunk_key(payload->key) || payload->generation.value == 0) {
		return WtApplicationStatus::InvalidInput;
	}
	return queue_.push(payload) ? WtApplicationStatus::Ok : WtApplicationStatus::QueueFull;
}

bool WtCollisionApplyQueue::pop(WtCollisionPayloadPtr &payload) {
	return queue_.pop(payload);
}

std::size_t WtCollisionApplyQueue::size() const noexcept {
	return queue_.size();
}

std::size_t WtCollisionApplyQueue::capacity() const noexcept {
	return queue_.capacity();
}

} // namespace world_transvoxel
