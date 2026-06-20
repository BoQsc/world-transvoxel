#pragma once

#include "core/wt_application_status.h"
#include "core/wt_bounded_fifo.h"
#include "render/wt_render_payload.h"

#include <cstddef>
#include <memory>

namespace world_transvoxel {

using WtRenderPayloadPtr = std::shared_ptr<const WtRenderPayload>;

struct WtRenderApplyEntry {
	WtRenderPayloadPtr payload;
	std::uint64_t submission_tick = 0;
};

class WtRenderSink {
public:
	virtual ~WtRenderSink() = default;
	virtual bool apply_render(const WtRenderPayload &payload) = 0;
};

class WtRenderApplyQueue {
public:
	explicit WtRenderApplyQueue(std::size_t capacity);

	WtApplicationStatus submit(const WtRenderPayloadPtr &payload, std::uint64_t submission_tick);
	bool pop(WtRenderApplyEntry &entry);
	std::size_t size() const noexcept;
	std::size_t capacity() const noexcept;

private:
	WtBoundedFifo<WtRenderApplyEntry> queue_;
};

} // namespace world_transvoxel
