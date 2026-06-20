#pragma once

#include "core/wt_application_status.h"
#include "core/wt_bounded_fifo.h"
#include "physics/wt_collision_builder.h"

#include <cstddef>
#include <memory>

namespace world_transvoxel {

using WtCollisionPayloadPtr = std::shared_ptr<const WtCollisionPayload>;

class WtCollisionSink {
public:
	virtual ~WtCollisionSink() = default;
	virtual bool apply_collision(const WtCollisionPayload &payload) = 0;
};

class WtCollisionApplyQueue {
public:
	explicit WtCollisionApplyQueue(std::size_t capacity);

	WtApplicationStatus submit(const WtCollisionPayloadPtr &payload);
	bool pop(WtCollisionPayloadPtr &payload);
	std::size_t size() const noexcept;
	std::size_t capacity() const noexcept;

private:
	WtBoundedFifo<WtCollisionPayloadPtr> queue_;
};

} // namespace world_transvoxel
