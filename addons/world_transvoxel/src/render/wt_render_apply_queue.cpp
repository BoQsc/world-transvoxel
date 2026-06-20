#include "render/wt_render_apply_queue.h"

namespace world_transvoxel {

WtRenderApplyQueue::WtRenderApplyQueue(std::size_t capacity) : queue_(capacity) {
}

WtApplicationStatus WtRenderApplyQueue::submit(const WtRenderPayloadPtr &payload) {
	if (!payload || !wt_is_valid_chunk_key(payload->key) || payload->generation.value == 0) {
		return WtApplicationStatus::InvalidInput;
	}
	return queue_.push(payload) ? WtApplicationStatus::Ok : WtApplicationStatus::QueueFull;
}

bool WtRenderApplyQueue::pop(WtRenderPayloadPtr &payload) {
	return queue_.pop(payload);
}

std::size_t WtRenderApplyQueue::size() const noexcept {
	return queue_.size();
}

std::size_t WtRenderApplyQueue::capacity() const noexcept {
	return queue_.capacity();
}

} // namespace world_transvoxel
