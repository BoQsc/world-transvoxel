#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace world_transvoxel {

class WtChunkApplicationService;
class WtGodotCollisionSink;
class WtGodotRenderSink;
struct WtCollisionPayload;
struct WtRenderPayload;

struct WtM5ApplicationFrameResult {
	bool valid = false;
	std::uint64_t duration_ns = 0;
	std::uint64_t render_sink_ns = 0;
	std::uint64_t collision_sink_ns = 0;
	std::size_t render_processed = 0;
	std::size_t collision_processed = 0;
	std::size_t ready_count = 0;
};

class WtM5ApplicationBenchmarkFixture {
public:
	bool prepare_batch(
		std::size_t render_count,
		std::size_t collision_count,
		std::uint64_t generation,
		WtChunkApplicationService &application
	);
	WtM5ApplicationFrameResult apply_frame(
		std::size_t render_budget,
		std::size_t collision_budget,
		WtChunkApplicationService &application,
		WtGodotRenderSink &render_sink,
		WtGodotCollisionSink &collision_sink
	);
	std::uint64_t clear(
		WtChunkApplicationService &application,
		WtGodotRenderSink &render_sink,
		WtGodotCollisionSink &collision_sink
	);

private:
	bool ensure_payloads();

	std::shared_ptr<const WtRenderPayload> base_render_;
	std::shared_ptr<const WtCollisionPayload> base_collision_;
	std::vector<std::int32_t> active_x_;
	std::size_t collision_count_ = 0;
};

} // namespace world_transvoxel
