#pragma once

#include "core/wt_application_status.h"
#include "physics/wt_collision_builder.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace world_transvoxel {

using WtCollisionPayloadPtr = std::shared_ptr<const WtCollisionPayload>;

struct WtCollisionApplyEntry {
	WtCollisionPayloadPtr payload;
	std::uint64_t submission_tick = 0;
	bool interaction_critical = false;
};

class WtCollisionSink {
public:
	virtual ~WtCollisionSink() = default;
	virtual bool apply_collision(const WtCollisionPayload &payload) = 0;
};

class WtCollisionApplyQueue {
public:
	explicit WtCollisionApplyQueue(std::size_t capacity);

	WtApplicationStatus submit(
		const WtCollisionPayloadPtr &payload,
		std::uint64_t submission_tick,
		bool interaction_critical = false
	);
	bool pop(WtCollisionApplyEntry &entry);
	std::size_t size() const noexcept;
	std::size_t capacity() const noexcept;

private:
	std::size_t capacity_ = 0;
	std::vector<WtCollisionApplyEntry> queue_;
};

} // namespace world_transvoxel
