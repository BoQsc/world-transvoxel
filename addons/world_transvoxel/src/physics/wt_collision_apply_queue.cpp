#include "physics/wt_collision_apply_queue.h"

namespace world_transvoxel {

WtCollisionApplyQueue::WtCollisionApplyQueue(std::size_t capacity) : queue_(capacity) {
}

WtApplicationStatus WtCollisionApplyQueue::submit(
	const WtCollisionPayloadPtr &payload,
	std::uint64_t submission_tick
) {
	if (!payload || !wt_is_valid_chunk_key(payload->key) || payload->generation.value == 0) {
		return WtApplicationStatus::InvalidInput;
	}
	return queue_.push({ payload, submission_tick }) ?
		WtApplicationStatus::Ok : WtApplicationStatus::QueueFull;
}

bool WtCollisionApplyQueue::pop(WtCollisionApplyEntry &entry) {
	return queue_.pop(entry);
}

std::size_t WtCollisionApplyQueue::size() const noexcept {
	return queue_.size();
}

std::size_t WtCollisionApplyQueue::capacity() const noexcept {
	return queue_.capacity();
}

} // namespace world_transvoxel
