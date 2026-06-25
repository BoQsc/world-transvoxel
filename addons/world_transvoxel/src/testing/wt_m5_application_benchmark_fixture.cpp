#include "testing/wt_m5_application_benchmark_fixture.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "meshing/wt_chunk_mesher.h"
#include "physics/wt_collision_builder.h"
#include "physics/wt_godot_collision_sink.h"
#include "render/wt_godot_render_sink.h"
#include "services/wt_chunk_application.h"

#include <chrono>

namespace world_transvoxel {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(Clock::time_point start) {
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			Clock::now() - start
		).count()
	);
}

struct BenchmarkSphereSource final : WtChunkSampleSource {
	bool sample(
		const WtGridPoint &point,
		WtScalarSample &output
	) const noexcept override {
		const double x = static_cast<double>(point.x - 8);
		const double y = static_cast<double>(point.y - 8);
		const double z = static_cast<double>(point.z - 8);
		const double density = x * x + y * y + z * z - 6.25 * 6.25;
		output.density = static_cast<float>(density);
		output.material = density < 0.0 ? 1 : 0;
		return true;
	}
};

class TimedRenderSink final : public WtRenderSink {
public:
	explicit TimedRenderSink(WtGodotRenderSink &sink) : sink_(sink) {
	}

	bool apply_render(const WtRenderPayload &payload) override {
		const Clock::time_point start = Clock::now();
		const bool applied = sink_.apply_render(payload);
		duration_ns += elapsed_ns(start);
		return applied;
	}

	std::uint64_t duration_ns = 0;

private:
	WtGodotRenderSink &sink_;
};

class TimedCollisionSink final : public WtCollisionSink {
public:
	explicit TimedCollisionSink(WtGodotCollisionSink &sink) : sink_(sink) {
	}

	bool apply_collision(const WtCollisionPayload &payload) override {
		const Clock::time_point start = Clock::now();
		const bool applied = sink_.apply_collision(payload);
		duration_ns += elapsed_ns(start);
		return applied;
	}

	std::uint64_t duration_ns = 0;

private:
	WtGodotCollisionSink &sink_;
};

} // namespace

bool WtM5ApplicationBenchmarkFixture::ensure_payloads() {
	if (base_render_ && base_collision_) {
		return true;
	}
	const WtChunkKey key = { 0, 0, 0, 0 };
	const WtChunkMesher mesher(wt_get_transvoxel_mit_backend());
	WtChunkMeshingScratch scratch;
	WtChunkMeshResult mesh;
	const BenchmarkSphereSource source;
	if (mesher.mesh({ key, 0, 0.0F, 0.25F }, source, mesh, scratch) !=
			WtChunkMeshingStatus::Ok) {
		return false;
	}
	auto render = std::make_shared<WtRenderPayload>();
	if (wt_build_render_payload(mesh, { 1 }, *render) !=
			WtRenderBuildStatus::Ok) {
		return false;
	}
	auto collision = std::make_shared<WtCollisionPayload>();
	if (wt_build_collision_payload(*render, {}, *collision) !=
			WtCollisionBuildStatus::Ok) {
		return false;
	}
	base_render_ = std::move(render);
	base_collision_ = std::move(collision);
	return true;
}

bool WtM5ApplicationBenchmarkFixture::prepare_batch(
	std::size_t render_count,
	std::size_t collision_count,
	std::uint64_t generation,
	WtChunkApplicationService &application
) {
	if (render_count == 0 || render_count > 64 ||
		collision_count > render_count || generation == 0 ||
		!ensure_payloads()) {
		return false;
	}
	active_x_.clear();
	active_x_.reserve(render_count);
	collision_count_ = collision_count;
	for (std::size_t index = 0; index < render_count; ++index) {
		const WtChunkKey key = {
			static_cast<std::int32_t>(index),
			0,
			0,
			0,
		};
		const WtGenerationToken token = { generation };
		if (application.expect_chunk(
				key,
				token,
				index < collision_count
			) != WtApplicationStatus::Ok) {
			return false;
		}
		auto render = std::make_shared<WtRenderPayload>(*base_render_);
		render->key = key;
		render->generation = token;
		render->world_origin = wt_chunk_bounds(key).minimum;
		if (application.submit_render(render) != WtApplicationStatus::Ok) {
			return false;
		}
		if (index < collision_count) {
			auto collision =
				std::make_shared<WtCollisionPayload>(*base_collision_);
			collision->key = key;
			collision->generation = token;
			collision->world_origin = render->world_origin;
			if (application.submit_collision(collision) !=
					WtApplicationStatus::Ok) {
				return false;
			}
		}
		active_x_.push_back(key.x);
	}
	return true;
}

WtM5ApplicationFrameResult
WtM5ApplicationBenchmarkFixture::apply_frame(
	std::size_t render_budget,
	std::size_t collision_budget,
	WtChunkApplicationService &application,
	WtGodotRenderSink &render_sink,
	WtGodotCollisionSink &collision_sink
) {
	TimedRenderSink timed_render(render_sink);
	TimedCollisionSink timed_collision(collision_sink);
	const Clock::time_point start = Clock::now();
	const WtApplicationBatchResult batch = application.apply(
		render_budget,
		collision_budget,
		timed_render,
		timed_collision
	);
	WtM5ApplicationFrameResult result;
	result.duration_ns = elapsed_ns(start);
	result.render_sink_ns = timed_render.duration_ns;
	result.collision_sink_ns = timed_collision.duration_ns;
	result.render_processed = batch.render_processed;
	result.collision_processed = batch.collision_processed;
	for (std::int32_t x : active_x_) {
		const WtChunkApplicationRecord *record =
			application.find_record({ x, 0, 0, 0 });
		result.ready_count +=
			record != nullptr && record->fully_ready() ? 1U : 0U;
	}
	result.valid =
		result.render_processed <= render_budget &&
		result.collision_processed <= collision_budget &&
		result.render_sink_ns + result.collision_sink_ns <=
			result.duration_ns;
	return result;
}

std::uint64_t WtM5ApplicationBenchmarkFixture::clear(
	WtChunkApplicationService &application,
	WtGodotRenderSink &render_sink,
	WtGodotCollisionSink &collision_sink
) {
	const Clock::time_point start = Clock::now();
	render_sink.clear();
	collision_sink.clear();
	for (std::size_t index = 0; index < active_x_.size(); ++index) {
		const WtChunkKey key = { active_x_[index], 0, 0, 0 };
		application.forget_chunk(key);
	}
	active_x_.clear();
	collision_count_ = 0;
	return elapsed_ns(start);
}

} // namespace world_transvoxel
