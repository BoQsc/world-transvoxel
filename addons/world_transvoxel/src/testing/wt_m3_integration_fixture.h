#pragma once

#include <cstdint>
#include <memory>

namespace world_transvoxel {

class WtChunkApplicationService;
class WtGodotCollisionSink;
class WtGodotRenderSink;
struct WtRenderPayload;

class WtM3IntegrationFixture {
public:
	bool submit_generation(
		std::int64_t generation,
		bool collision_required,
		WtChunkApplicationService &application
	);
	bool set_collision_distance(
		double distance,
		WtChunkApplicationService &application,
		WtGodotCollisionSink &collision_sink
	);
	bool fully_ready(const WtChunkApplicationService &application) const noexcept;
	std::int64_t render_generation(const WtGodotRenderSink &sink) const noexcept;
	std::int64_t collision_generation(const WtGodotCollisionSink &sink) const noexcept;
	std::int64_t stale_render_count(
		const WtChunkApplicationService &application
	) const noexcept;
	std::int64_t stale_collision_count(
		const WtChunkApplicationService &application
	) const noexcept;
	void forget(
		WtChunkApplicationService &application,
		WtGodotRenderSink &render_sink,
		WtGodotCollisionSink &collision_sink
	);

private:
	std::shared_ptr<const WtRenderPayload> render_payload_;
};

} // namespace world_transvoxel
