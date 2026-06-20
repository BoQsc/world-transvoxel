#include "physics/wt_collision_builder.h"
#include "services/wt_chunk_application.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>

namespace wt = world_transvoxel;

namespace {

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

wt::WtCellVertex vertex(float x, float y, float z, std::uint16_t material) {
	return { { x, y, z }, { 0.0F, 1.0F, 0.0F }, material, 0, 0 };
}

void add_triangle(
	wt::WtChunkMeshBuffer &buffer,
	const wt::WtCellVertex &a,
	const wt::WtCellVertex &b,
	const wt::WtCellVertex &c
) {
	const std::uint32_t base = static_cast<std::uint32_t>(buffer.vertices.size());
	buffer.vertices.push_back(a);
	buffer.vertices.push_back(b);
	buffer.vertices.push_back(c);
	buffer.indices.push_back(base);
	buffer.indices.push_back(base + 1);
	buffer.indices.push_back(base + 2);
}

wt::WtChunkMeshResult make_chunk_mesh() {
	wt::WtChunkMeshResult mesh;
	mesh.key = { 0, 0, 0, 1 };
	mesh.world_origin = wt::wt_chunk_bounds(mesh.key).minimum;
	mesh.transition_mask = wt::wt_face_bit(wt::WtChunkFace::PositiveX);
	add_triangle(
		mesh.regular,
		vertex(1.0F, 0.0F, 1.0F, 3),
		vertex(2.0F, 0.0F, 1.0F, 3),
		vertex(1.0F, 0.0F, 2.0F, 3)
	);
	add_triangle(
		mesh.transitions[static_cast<std::size_t>(wt::WtChunkFace::PositiveX)],
		vertex(32.0F, 1.0F, 1.0F, 5),
		vertex(32.0F, 1.0F, 2.0F, 5),
		vertex(32.0F, 2.0F, 1.0F, 5)
	);
	return mesh;
}

void test_render_builder(wt::WtRenderPayload &render) {
	const wt::WtChunkMeshResult mesh = make_chunk_mesh();
	check(wt::wt_build_render_payload(mesh, { 7 }, render) ==
		wt::WtRenderBuildStatus::Ok, "valid render payload failed");
	check(render.vertices.size() == 6 && render.indices.size() == 6,
		"render payload did not combine regular and transition buffers");
	check(render.vertices[0].material == 3 && render.vertices[3].material == 5,
		"render payload lost categorical materials");

	wt::WtChunkMeshResult invalid = mesh;
	add_triangle(
		invalid.transitions[0],
		vertex(0.0F, 0.0F, 0.0F, 1),
		vertex(1.0F, 0.0F, 0.0F, 1),
		vertex(0.0F, 1.0F, 0.0F, 1)
	);
	check(wt::wt_build_render_payload(invalid, { 8 }, render) ==
		wt::WtRenderBuildStatus::InvalidMesh,
		"inactive transition output was accepted");
	check(render.vertices.empty() && render.indices.empty(),
		"failed render build retained output");
	check(wt::wt_build_render_payload(mesh, { 7 }, render) ==
		wt::WtRenderBuildStatus::Ok, "render payload rebuild failed");
}

void test_collision_builder() {
	wt::WtRenderPayload render;
	render.key = { 0, 0, 0, 0 };
	render.generation = { 11 };
	render.world_origin = wt::wt_chunk_bounds(render.key).minimum;
	render.vertices = {
		{ { 0.0F, 0.0F, 0.0F }, {}, 1 },
		{ { 1.0F, 0.0F, 0.0F }, {}, 1 },
		{ { 0.0F, 1.0F, 0.0F }, {}, 1 },
		{ { 2.0F, 0.0F, 0.0F }, {}, 1 },
		{ { 3.0F, 0.0F, 0.0F }, {}, 1 },
		{ { 4.0F, 0.0F, 0.0F }, {}, 1 },
		{ { 5.0F, 0.0F, 0.0F }, {}, 1 },
		{ { 6.0F, 0.0F, 0.0F }, {}, 1 },
		{ { 6.0F, 0.0000001F, 0.0F }, {}, 1 },
	};
	render.indices = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
	wt::WtCollisionPayload collision;
	const wt::WtCollisionPolicy policy;
	check(wt::wt_build_collision_payload(render, policy, collision) ==
		wt::WtCollisionBuildStatus::Ok, "collision sanitation failed");
	check(collision.faces.size() == 3, "collision sanitation retained invalid faces");
	check(collision.metrics.input_triangles == 3 &&
		collision.metrics.output_triangles == 1 &&
		collision.metrics.degenerate_triangles == 1 &&
		collision.metrics.thin_triangles == 1,
		"collision sanitation metrics mismatch");

	check(wt::wt_evaluate_collision_requirement(policy, false, 96.0) ==
		wt::WtCollisionRequirement::Required, "collision activation boundary failed");
	check(wt::wt_evaluate_collision_requirement(policy, false, 96.01) ==
		wt::WtCollisionRequirement::NotRequired, "collision activation range failed");
	check(wt::wt_evaluate_collision_requirement(policy, true, 127.99) ==
		wt::WtCollisionRequirement::Required, "collision hysteresis failed");
	check(wt::wt_evaluate_collision_requirement(policy, true, 128.01) ==
		wt::WtCollisionRequirement::NotRequired, "collision deactivation failed");
	check(wt::wt_evaluate_collision_requirement(
		policy, false, std::numeric_limits<double>::quiet_NaN()) ==
		wt::WtCollisionRequirement::Invalid, "non-finite collision distance accepted");
}

struct RenderSink final : wt::WtRenderSink {
	std::size_t calls = 0;
	bool fail = false;

	bool apply_render(const wt::WtRenderPayload &) override {
		++calls;
		return !fail;
	}
};

struct CollisionSink final : wt::WtCollisionSink {
	std::size_t calls = 0;
	bool fail = false;

	bool apply_collision(const wt::WtCollisionPayload &) override {
		++calls;
		return !fail;
	}
};

void test_application_service(
	const wt::WtRenderPayload &render_source,
	std::size_t stale_cycles
) {
	wt::WtChunkApplicationService service(2, 2, 2);
	RenderSink render_sink;
	CollisionSink collision_sink;
	const wt::WtChunkKey key = render_source.key;
	auto render1 = std::make_shared<wt::WtRenderPayload>(render_source);
	render1->generation = { 1 };
	auto collision1 = std::make_shared<wt::WtCollisionPayload>();
	check(wt::wt_build_collision_payload(*render1, {}, *collision1) ==
		wt::WtCollisionBuildStatus::Ok, "application collision payload failed");
	check(service.expect_chunk(key, { 1 }, true) == wt::WtApplicationStatus::Ok,
		"initial application expectation failed");
	check(service.submit_render(render1) == wt::WtApplicationStatus::Ok &&
		service.submit_collision(collision1) == wt::WtApplicationStatus::Ok,
		"initial application submission failed");
	check(service.expect_chunk(key, { 2 }, true) == wt::WtApplicationStatus::Ok,
		"application supersession failed");
	const wt::WtApplicationBatchResult stale = service.apply(
		1, 1, render_sink, collision_sink
	);
	check(stale.render_processed == 1 && stale.collision_processed == 1,
		"stale application did not consume frame budget");
	check(render_sink.calls == 0 && collision_sink.calls == 0,
		"stale application reached a resource sink");

	auto render2 = std::make_shared<wt::WtRenderPayload>(render_source);
	render2->generation = { 2 };
	auto collision2 = std::make_shared<wt::WtCollisionPayload>();
	check(wt::wt_build_collision_payload(*render2, {}, *collision2) ==
		wt::WtCollisionBuildStatus::Ok, "current collision payload failed");
	check(service.submit_render(render2) == wt::WtApplicationStatus::Ok &&
		service.submit_collision(collision2) == wt::WtApplicationStatus::Ok,
		"current application submission failed");
	service.apply(0, 1, render_sink, collision_sink);
	const wt::WtChunkApplicationRecord *record = service.find_record(key);
	check(record != nullptr && !record->visual_ready && record->collision_ready &&
		!record->fully_ready(), "independent collision readiness failed");
	service.apply(1, 0, render_sink, collision_sink);
	record = service.find_record(key);
	check(record != nullptr && record->fully_ready(), "full readiness failed");

	for (std::size_t cycle = 0; cycle < stale_cycles; ++cycle) {
		const std::uint64_t generation = 3 + static_cast<std::uint64_t>(cycle) * 2;
		auto stale_render = std::make_shared<wt::WtRenderPayload>(render_source);
		stale_render->generation = { generation };
		check(service.expect_chunk(key, { generation }, false) ==
			wt::WtApplicationStatus::Ok, "cycle expectation failed");
		check(service.submit_render(stale_render) == wt::WtApplicationStatus::Ok,
			"cycle submission failed");
		check(service.expect_chunk(key, { generation + 1 }, false) ==
			wt::WtApplicationStatus::Ok, "cycle supersession failed");
		service.apply(1, 0, render_sink, collision_sink);
		check(service.queued_render_count() == 0 && service.get_records().size() == 1,
			"application state grew during supersession cycles");
	}

	wt::WtChunkApplicationService bounded(1, 1, 1);
	check(bounded.submit_render(render1) == wt::WtApplicationStatus::Ok,
		"bounded render queue rejected first item");
	check(bounded.submit_render(render1) == wt::WtApplicationStatus::QueueFull,
		"bounded render queue accepted overflow");
	check(bounded.expect_chunk(key, { 1 }, false) == wt::WtApplicationStatus::Ok,
		"bounded application record failed");
	check(bounded.expect_chunk({ 1, 0, 0, 0 }, { 2 }, false) ==
		wt::WtApplicationStatus::RecordCapacityExceeded,
		"bounded application records accepted overflow");

	const wt::WtApplicationMetrics metrics = service.get_metrics();
	check(metrics.stale_render == stale_cycles + 1 && metrics.stale_collision == 1,
		"stale application metrics mismatch");
	check(metrics.applied_render == 1 && metrics.applied_collision == 1,
		"applied resource metrics mismatch");
	check(metrics.render_latency_frames_total == stale_cycles + 3 &&
		metrics.render_latency_frames_maximum == 2,
		"render application latency metrics mismatch");
	check(metrics.collision_latency_frames_total == 2 &&
		metrics.collision_latency_frames_maximum == 1,
		"collision application latency metrics mismatch");
}

} // namespace

int main() {
	wt::WtRenderPayload render;
	test_render_builder(render);
	test_collision_builder();
	constexpr std::size_t stale_cycles = 1000;
	test_application_service(render, stale_cycles);
	if (failure_count != 0) {
		std::fprintf(stderr, "M3_APPLICATION_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M3_APPLICATION_PASS stale_cycles=%zu\n", stale_cycles);
	return 0;
}
