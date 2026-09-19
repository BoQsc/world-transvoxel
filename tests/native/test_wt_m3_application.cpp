#include "physics/wt_collision_builder.h"
#include "services/wt_chunk_application.h"
#include "services/wt_chunk_publication_policy.h"

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>

namespace wt = world_transvoxel;

namespace {

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

wt::WtCellVertex vertex(
	float x,
	float y,
	float z,
	std::uint16_t material,
	bool material_authored = false
) {
	return {
		{ x, y, z },
		{ 0.0F, 1.0F, 0.0F },
		material,
		material_authored,
		0,
		0,
	};
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
		vertex(1.0F, 0.0F, 1.0F, 3, true),
		vertex(2.0F, 0.0F, 1.0F, 3, true),
		vertex(1.0F, 0.0F, 2.0F, 3, true)
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
	check(render.vertices[0].material_authored &&
		!render.vertices[3].material_authored,
		"render payload lost material-authoring provenance");

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

	wt::WtChunkMeshResult water = mesh;
	water.regular.vertices.clear();
	water.regular.indices.clear();
	for (wt::WtChunkMeshBuffer &transition : water.transitions) {
		transition.vertices.clear();
		transition.indices.clear();
	}
	add_triangle(
		water.regular,
		vertex(4.0F, 1.0F, 4.0F, 9),
		vertex(5.0F, 1.0F, 4.0F, 9),
		vertex(4.0F, 1.0F, 5.0F, 9)
	);
	add_triangle(
		water.regular,
		{ { 6.0F, 0.0F, 4.0F }, { 1.0F, 0.0F, 0.0F }, 9, true, 0, 0 },
		{ { 6.0F, 1.0F, 4.0F }, { 1.0F, 0.0F, 0.0F }, 9, true, 0, 0 },
		{ { 6.0F, 0.0F, 5.0F }, { 1.0F, 0.0F, 0.0F }, 9, true, 0, 0 }
	);
	add_triangle(
		water.regular,
		{ { 8.0F, 1.0F, 4.0F }, { 0.0F, 1.0F, 0.0F }, 9, 0, 0 },
		{ { 9.0F, 0.0F, 4.0F }, { 0.0F, 1.0F, 0.0F }, 9, 0, 0 },
		{ { 8.0F, 1.0F, 5.0F }, { 0.0F, 1.0F, 0.0F }, 9, 0, 0 }
	);
	add_triangle(
		water.regular,
		{ { 10.0F, 4.0F, 4.0F }, { 0.0F, 1.0F, 0.0F }, 9, 0, 0 },
		{ { 11.0F, 0.0F, 4.0F }, { 0.0F, 1.0F, 0.0F }, 9, 0, 0 },
		{ { 10.0F, 4.0F, 5.0F }, { 0.0F, 1.0F, 0.0F }, 9, 0, 0 }
	);
	check(wt::wt_build_render_payload(mesh, water, { 7 }, render) ==
		wt::WtRenderBuildStatus::Ok, "water render payload failed");
	check(render.vertices.size() == 6 && render.indices.size() == 6 &&
		render.water_vertices.size() == 12 && render.water_indices.size() == 12,
		"water payload did not retain its volumetric boundary geometry");
	check(render.water_vertices[0].material == 9,
		"water render surface lost its material identity");
	check(render.water_vertices[0].normal.y == 1.0F &&
		render.water_vertices[3].normal.x == 1.0F &&
		render.water_vertices[3].position.y == 0.0F &&
		render.water_vertices[4].position.y == 1.0F,
		"authored water boundary was flattened into a free-surface cap");
	check(render.water_vertices[0].normal.y == 1.0F,
		"water free surface did not receive the authoritative gravity normal");
	check(render.water_vertices[7].position.y == 0.0F &&
		render.water_vertices[10].position.y == 0.0F &&
		!render.water_vertices[6].material_authored &&
		!render.water_vertices[9].material_authored,
		"generated water boundary was modified by render-payload construction");
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
	render.water_vertices = {
		{ { 10.0F, 0.0F, 0.0F }, {}, 9 },
		{ { 11.0F, 0.0F, 0.0F }, {}, 9 },
		{ { 10.0F, 1.0F, 0.0F }, {}, 9 },
	};
	render.water_indices = { 0, 1, 2 };
	wt::WtCollisionPayload collision;
	const wt::WtCollisionPolicy policy;
	check(wt::wt_build_collision_payload(render, policy, collision) ==
		wt::WtCollisionBuildStatus::Ok, "collision sanitation failed");
	check(collision.faces.size() == 3, "collision sanitation retained invalid faces");
	check(collision.metrics.input_triangles == 3,
		"water surface leaked into solid collision input");
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

	const wt::WtChunkMeshResult transition_mesh = make_chunk_mesh();
	wt::WtCollisionPayload regular_collision;
	check(wt::wt_build_regular_collision_payload(
			transition_mesh, { 12 }, policy, regular_collision
		) == wt::WtCollisionBuildStatus::Ok,
		"regular-only collision build failed");
	check(regular_collision.metrics.input_triangles == 1 &&
		regular_collision.metrics.output_triangles == 1 &&
		regular_collision.faces.size() == 3,
		"transition seam geometry leaked into regular collision");

	wt::WtChunkMeshResult detailed_mesh;
	detailed_mesh.key = { 0, 0, 0, 0 };
	detailed_mesh.world_origin = wt::wt_chunk_bounds(detailed_mesh.key).minimum;
	for (std::size_t triangle = 0; triangle < 513; ++triangle) {
		const float x = static_cast<float>(triangle % 27U) * 0.5F;
		const float y = static_cast<float>((triangle / 27U) % 19U) * 0.5F;
		const float z = static_cast<float>(triangle / (27U * 19U)) * 0.5F;
		add_triangle(
			detailed_mesh.regular,
			vertex(x, y, z, 1),
			vertex(x + 0.25F, y, z, 1),
			vertex(x, y + 0.25F, z, 1)
		);
	}
	wt::WtCollisionPayload detailed_collision;
	check(wt::wt_build_regular_collision_payload(
			detailed_mesh, { 13 }, policy, detailed_collision
		) == wt::WtCollisionBuildStatus::Ok,
		"detailed regular collision build failed");
	check(detailed_collision.metrics.input_triangles == 513 &&
		detailed_collision.metrics.output_triangles == 513 &&
		detailed_collision.metrics.decimated_triangles == 0 &&
		detailed_collision.faces.size() == 513 * 3,
		"default collision policy removed valid regular terrain triangles");

	wt::WtChunkMeshResult block_mesh;
	block_mesh.key = { 0, 0, 0, 0 };
	block_mesh.world_origin = wt::wt_chunk_bounds(block_mesh.key).minimum;
	add_triangle(block_mesh.regular,
		vertex(1.0F, 1.0F, 1.0F, 1),
		vertex(2.0F, 1.0F, 1.0F, 1),
		vertex(1.0F, 2.0F, 1.0F, 1));
	add_triangle(block_mesh.regular,
		vertex(13.0F, 13.0F, 13.0F, 1),
		vertex(14.0F, 13.0F, 13.0F, 1),
		vertex(13.0F, 14.0F, 13.0F, 1));
	wt::WtCollisionPayload block_patch;
	check(wt::wt_build_regular_collision_patch(
			block_mesh, { 14 }, policy, 1U << 0, block_patch
		) == wt::WtCollisionBuildStatus::Ok,
		"incremental collision block build failed");
	check(block_patch.incremental_patch && block_patch.dirty_block_mask == 1U &&
		block_patch.metrics.input_triangles == 1 &&
		block_patch.metrics.output_triangles == 1 &&
		block_patch.blocks[0].face_count == 3 &&
		block_patch.blocks[7].face_count == 0 &&
		block_patch.faces.size() == 3,
		"incremental collision block ownership was not isolated");
	wt::WtCollisionPayload block_base;
	check(wt::wt_build_regular_collision_payload(
			block_mesh, { 13 }, policy, block_base
		) == wt::WtCollisionBuildStatus::Ok,
		"collision block merge base build failed");
	wt::WtCollisionPayload merged_blocks;
	check(wt::wt_merge_collision_patch(
			block_base, block_patch, merged_blocks
		) == wt::WtCollisionBuildStatus::Ok &&
		merged_blocks.generation.value == 14 &&
		!merged_blocks.incremental_patch &&
		merged_blocks.dirty_block_mask == wt::kWtCollisionAllBlocksMask &&
		merged_blocks.blocks[0].face_count == 3 &&
		merged_blocks.blocks[7].face_count == 3 &&
		merged_blocks.faces.size() == 6,
		"collision block merge did not preserve clean authoritative blocks");
	wt::WtChunkMeshResult empty_block_mesh;
	empty_block_mesh.key = block_mesh.key;
	empty_block_mesh.world_origin = block_mesh.world_origin;
	wt::WtCollisionPayload empty_patch;
	check(wt::wt_build_regular_collision_patch(
			empty_block_mesh, { 15 }, policy, 1U, empty_patch
		) == wt::WtCollisionBuildStatus::Ok &&
		wt::wt_merge_collision_patch(
			merged_blocks, empty_patch, block_base
		) == wt::WtCollisionBuildStatus::Ok &&
		block_base.blocks[0].face_count == 0 &&
		block_base.blocks[7].face_count == 3 &&
		block_base.faces.size() == 3,
		"empty collision patch did not retire only its dirty block");
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

struct SlowCollisionSink final : wt::WtCollisionSink {
	std::size_t calls = 0;

	bool apply_collision(const wt::WtCollisionPayload &) override {
		++calls;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		return true;
	}
};

void test_interaction_collision_apply_priority() {
	wt::WtCollisionApplyQueue queue(3);
	const auto payload = [](std::int32_t x) {
		auto result = std::make_shared<wt::WtCollisionPayload>();
		result->key = { x, 0, 0, 0 };
		result->generation = { 1 };
		return result;
	};
	check(
		queue.submit(payload(1), 1) == wt::WtApplicationStatus::Ok &&
		queue.submit(payload(2), 2) == wt::WtApplicationStatus::Ok &&
		queue.submit(payload(3), 3, true) == wt::WtApplicationStatus::Ok,
		"interaction collision priority fixture submission failed"
	);
	wt::WtCollisionApplyEntry first;
	wt::WtCollisionApplyEntry second;
	wt::WtCollisionApplyEntry third;
	check(
		queue.pop(first) && first.interaction_critical &&
			first.payload->key.x == 3 &&
		queue.pop(second) && second.payload->key.x == 1 &&
		queue.pop(third) && third.payload->key.x == 2 &&
		queue.size() == 0,
		"interaction collision did not bypass the background apply backlog"
	);
}

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
	check(service.expect_chunk(key, { 1 }, true, true, false, false, 41) ==
		wt::WtApplicationStatus::Ok,
		"initial application expectation failed");
	{
		wt::WtChunkApplicationRecord revision_record;
		check(service.copy_record(key, revision_record) &&
			revision_record.world_revision == 41,
			"application expectation lost its world revision");
	}
	check(service.submit_render(render1) == wt::WtApplicationStatus::Ok &&
		service.submit_collision(collision1) == wt::WtApplicationStatus::Ok,
		"initial application submission failed");
	check(service.expect_chunk(key, { 2 }, true, true, false, false, 42) ==
		wt::WtApplicationStatus::Ok,
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

	wt::WtChunkApplicationService promotion(1, 2, 1);
	RenderSink promotion_render_sink;
	CollisionSink promotion_collision_sink;
	auto authority_render = std::make_shared<wt::WtRenderPayload>(render_source);
	authority_render->generation = { 31 };
	check(promotion.expect_chunk(key, { 31 }, false) ==
			wt::WtApplicationStatus::Ok &&
		promotion.submit_render(authority_render) == wt::WtApplicationStatus::Ok,
		"GPU candidate promotion authority setup failed");
	promotion.apply(1, 0, promotion_render_sink, promotion_collision_sink);
	auto gpu_candidate = std::make_shared<wt::WtRenderPayload>(*authority_render);
	gpu_candidate->publication_source =
		wt::WtRenderPublicationSource::GpuCellCandidate;
	check(promotion.submit_render(gpu_candidate) == wt::WtApplicationStatus::Ok,
		"same-generation GPU candidate submission failed");
	promotion.apply(1, 0, promotion_render_sink, promotion_collision_sink);
	const wt::WtApplicationMetrics promotion_metrics = promotion.get_metrics();
	check(promotion_render_sink.calls == 2 && promotion_collision_sink.calls == 0,
		"GPU candidate promotion changed collision application");
	check(promotion_metrics.submitted_gpu_candidate_render == 1 &&
		promotion_metrics.applied_gpu_candidate_render == 1 &&
		promotion_metrics.stale_gpu_candidate_render == 0,
		"GPU candidate publication provenance was not retained");

	wt::WtChunkApplicationService collision_only(1, 1, 1);
	RenderSink hidden_render_sink;
	CollisionSink collision_only_sink;
	check(collision_only.expect_chunk(key, { 1 }, true, false) ==
		wt::WtApplicationStatus::Ok,
		"collision-only expectation failed");
	auto collision_only_payload = std::make_shared<wt::WtCollisionPayload>();
	check(wt::wt_build_collision_payload(
			*render1, {}, *collision_only_payload
		) == wt::WtCollisionBuildStatus::Ok &&
		collision_only.submit_collision(collision_only_payload) ==
			wt::WtApplicationStatus::Ok,
		"collision-only payload submission failed");
	collision_only.apply(0, 1, hidden_render_sink, collision_only_sink);
	const wt::WtChunkApplicationRecord *collision_only_record =
		collision_only.find_record(key);
	check(collision_only_record != nullptr &&
		!collision_only_record->visual_required &&
		collision_only_record->collision_ready &&
		collision_only_record->fully_ready(),
		"collision-only readiness incorrectly waited for render");
	check(collision_only.set_visual_required(key, true) ==
		wt::WtApplicationStatus::Ok &&
		!collision_only.find_record(key)->fully_ready(),
		"collision-only visual promotion did not reset visual readiness");
}

void test_staged_replacement_collision_waits_for_render(
	const wt::WtRenderPayload &render_source
) {
	wt::WtChunkApplicationService service(1, 2, 2);
	RenderSink render_sink;
	CollisionSink collision_sink;
	const wt::WtChunkKey key = render_source.key;

	auto render1 = std::make_shared<wt::WtRenderPayload>(render_source);
	render1->generation = { 1 };
	auto collision1 = std::make_shared<wt::WtCollisionPayload>();
	check(wt::wt_build_collision_payload(*render1, {}, *collision1) ==
		wt::WtCollisionBuildStatus::Ok,
		"staged replacement initial collision payload failed");
	check(service.expect_chunk(key, { 1 }, true) == wt::WtApplicationStatus::Ok,
		"staged replacement initial expectation failed");
	check(service.submit_render(render1) == wt::WtApplicationStatus::Ok &&
		service.submit_collision(collision1) == wt::WtApplicationStatus::Ok,
		"staged replacement initial submission failed");
	service.apply(2, 2, render_sink, collision_sink);
	const wt::WtChunkApplicationRecord *initial_record =
		service.find_record(key);
	check(render_sink.calls == 1 && collision_sink.calls == 1 &&
		initial_record != nullptr && initial_record->fully_ready() &&
		initial_record->visual_generation.value == 1 &&
		initial_record->collision_generation.value == 1,
		"staged replacement initial resources did not become ready");

	auto render2 = std::make_shared<wt::WtRenderPayload>(render_source);
	render2->generation = { 2 };
	auto collision2 = std::make_shared<wt::WtCollisionPayload>();
	check(wt::wt_build_collision_payload(*render2, {}, *collision2) ==
		wt::WtCollisionBuildStatus::Ok,
		"staged replacement current collision payload failed");
	check(service.expect_chunk(key, { 2 }, true, true, true) ==
		wt::WtApplicationStatus::Ok,
		"staged replacement expectation failed");
	check(service.submit_collision(collision2) == wt::WtApplicationStatus::Ok,
		"staged replacement early collision submission failed");
	const wt::WtApplicationBatchResult deferred =
		service.apply(0, 1, render_sink, collision_sink);
	const wt::WtChunkApplicationRecord *record = service.find_record(key);
	check(deferred.collision_processed == 1 && collision_sink.calls == 1 &&
		record != nullptr && !record->visual_ready &&
		!record->collision_ready && !record->fully_ready(),
		"staged replacement collision applied before render readiness");

	check(service.submit_render(render2) == wt::WtApplicationStatus::Ok,
		"staged replacement render submission failed");
	const wt::WtApplicationBatchResult synchronized =
		service.apply(1, 1, render_sink, collision_sink);
	record = service.find_record(key);
	check(synchronized.render_processed == 1 &&
		synchronized.collision_processed == 1 &&
		render_sink.calls == 2 && collision_sink.calls == 2 &&
		record != nullptr && record->fully_ready() &&
		!record->staged_replacement &&
		record->visual_generation.value == 2 &&
		record->collision_generation.value == 2,
		"staged replacement did not synchronize render and collision");

	auto render3 = std::make_shared<wt::WtRenderPayload>(render_source);
	render3->generation = { 3 };
	auto collision3 = std::make_shared<wt::WtCollisionPayload>();
	check(wt::wt_build_collision_payload(*render3, {}, *collision3) ==
		wt::WtCollisionBuildStatus::Ok,
		"preserved replacement collision payload failed");
	check(service.expect_chunk(key, { 3 }, true, true, true, true, 1) ==
		wt::WtApplicationStatus::Ok,
		"preserved replacement expectation failed");
	record = service.find_record(key);
	check(record != nullptr && record->collision_ready &&
		record->collision_generation.value == 2 &&
		record->visual_generation.value == 0 &&
		!record->fully_ready(),
		"preserved prior collision was mistaken for current readiness");
	check(service.submit_render(render3) == wt::WtApplicationStatus::Ok,
		"preserved replacement render submission failed");
	service.apply(1, 0, render_sink, collision_sink);
	record = service.find_record(key);
	check(record != nullptr && record->visual_generation.value == 3 &&
		record->collision_generation.value == 2 &&
		!record->fully_ready() && record->staged_replacement,
		"replacement became ready before its collision generation");
	check(service.submit_collision(collision3) == wt::WtApplicationStatus::Ok,
		"preserved replacement collision submission failed");
	service.apply(0, 1, render_sink, collision_sink);
	record = service.find_record(key);
	check(record != nullptr && record->fully_ready() &&
		record->visual_generation.value == 3 &&
		record->collision_generation.value == 3 &&
		!record->staged_replacement,
		"preserved replacement generations did not synchronize");

	auto collision4 = std::make_shared<wt::WtCollisionPayload>(*collision3);
	collision4->generation = { 4 };
	check(service.expect_chunk(key, { 4 }, true, true, true, true, 2) ==
			wt::WtApplicationStatus::Ok &&
		service.submit_collision(collision4, true) ==
			wt::WtApplicationStatus::Ok,
		"interaction-critical replacement collision submission failed");
	service.apply(0, 1, render_sink, collision_sink);
	record = service.find_record(key);
	check(record != nullptr && record->collision_ready &&
		record->collision_generation.value == 4 &&
		!record->visual_ready && record->staged_replacement &&
		collision_sink.calls == 4 && service.deferred_collision_count() == 0,
		"interaction-critical collision waited for visual readiness");
}

void test_collision_revision_is_independent_from_visual_generation(
	const wt::WtRenderPayload &render_source
) {
	wt::WtChunkApplicationService service(1, 1, 2);
	RenderSink render_sink;
	CollisionSink collision_sink;
	const wt::WtChunkKey key = render_source.key;
	auto collision1 = std::make_shared<wt::WtCollisionPayload>();
	wt::WtRenderPayload generation1 = render_source;
	generation1.generation = { 1 };
	check(wt::wt_build_collision_payload(generation1, {}, *collision1) ==
		wt::WtCollisionBuildStatus::Ok,
		"collision revision fixture build failed");
	check(service.expect_chunk(key, { 1 }, true, false, false, false, 41) ==
			wt::WtApplicationStatus::Ok &&
		service.submit_collision(collision1) == wt::WtApplicationStatus::Ok,
		"collision revision fixture setup failed");
	service.apply(0, 1, render_sink, collision_sink);
	const wt::WtChunkApplicationRecord *record = service.find_record(key);
	check(record != nullptr && record->collision_current() &&
		!record->collision_work_required() &&
		record->collision_world_revision == 41,
		"applied collision did not retain its authoritative world revision");

	check(service.expect_chunk(key, { 2 }, true, false, false, true, 41) ==
		wt::WtApplicationStatus::Ok,
		"same-revision visual generation supersession failed");
	record = service.find_record(key);
	check(record != nullptr && record->collision_generation.value == 1 &&
		record->generation.value == 2 && record->collision_current() &&
		!record->collision_work_required() &&
		record->fully_ready(),
		"same world revision incorrectly invalidated authoritative collision");

	check(service.expect_chunk(key, { 3 }, true, false, false, true, 42) ==
		wt::WtApplicationStatus::Ok,
		"edited world revision supersession failed");
	record = service.find_record(key);
	check(record != nullptr && record->collision_ready &&
		!record->collision_current() && record->collision_work_required() &&
		!record->fully_ready(),
		"new world revision accepted stale physical collision as current");
	check(service.submit_collision(collision1) == wt::WtApplicationStatus::Ok,
		"stale collision revision submission failed");
	service.apply(0, 1, render_sink, collision_sink);
	record = service.find_record(key);
	check(record != nullptr && !record->collision_current() &&
		collision_sink.calls == 1,
		"stale collision payload restored current collision state");

	wt::WtRenderPayload generation3 = render_source;
	generation3.generation = { 3 };
	auto collision3 = std::make_shared<wt::WtCollisionPayload>();
	check(wt::wt_build_collision_payload(generation3, {}, *collision3) ==
			wt::WtCollisionBuildStatus::Ok &&
		service.submit_collision(collision3) == wt::WtApplicationStatus::Ok,
		"current collision revision submission failed");
	service.apply(0, 1, render_sink, collision_sink);
	record = service.find_record(key);
	check(record != nullptr && record->collision_current() &&
		record->collision_world_revision == 42 && record->fully_ready(),
		"matching edited collision did not become authoritative");

	wt::WtChunkApplicationService acknowledgement_service(4, 4, 4);
	check(acknowledgement_service.expect_chunk(
			key, {1}, true, false, false, false, 51
		) == wt::WtApplicationStatus::Ok &&
		acknowledgement_service.expect_chunk(
			key, {2}, true, true, true, false, 51
		) == wt::WtApplicationStatus::Ok &&
		acknowledgement_service.acknowledge_collision_residency(
			key, {1}, 51
		) == wt::WtApplicationStatus::Ok,
		"late frontend collision acknowledgement was rejected");
	record = acknowledgement_service.find_record(key);
	check(record != nullptr && record->collision_current() &&
		record->collision_generation.value == 1 &&
		!record->collision_work_required(),
		"matching predecessor collision residency did not satisfy successor");
	check(acknowledgement_service.acknowledge_collision_residency(
			key, {1}, 50
		) == wt::WtApplicationStatus::StaleGeneration,
		"stale frontend collision revision was accepted");
}

void test_gpu_placeholder_waits_for_external_activation(
	const wt::WtRenderPayload &render_source
) {
	wt::WtChunkApplicationService service(1, 1, 1);
	RenderSink render_sink;
	CollisionSink collision_sink;
	auto placeholder = std::make_shared<wt::WtRenderPayload>(render_source);
	placeholder->generation = { 41 };
	placeholder->publication_source =
		wt::WtRenderPublicationSource::GpuResidentPlaceholder;
	check(service.expect_chunk(placeholder->key, { 41 }, false, true, true) ==
			wt::WtApplicationStatus::Ok &&
		service.submit_render(placeholder) == wt::WtApplicationStatus::Ok,
		"GPU placeholder setup failed");
	service.apply(1, 0, render_sink, collision_sink);
	const wt::WtChunkApplicationRecord *record =
		service.find_record(placeholder->key);
	check(record != nullptr && !record->visual_ready &&
		record->visual_generation.value == 41 &&
		record->external_visual_activation_required &&
		record->staged_replacement && !record->fully_ready(),
		"GPU placeholder retired coverage before external activation");
	wt::WtChunkApplicationService immediate(1, 1, 1);
	RenderSink immediate_render_sink;
	check(immediate.expect_chunk(
			placeholder->key, { 41 }, false, true, true
		) == wt::WtApplicationStatus::Ok &&
		immediate.apply_gpu_resident_placeholder(
			placeholder, immediate_render_sink
		) == wt::WtApplicationStatus::Ok &&
		immediate.queued_render_count() == 0,
		"GPU placeholder fast path entered the CPU render queue");
	const wt::WtChunkApplicationRecord *immediate_record =
		immediate.find_record(placeholder->key);
	check(immediate_record != nullptr &&
		immediate_record->external_visual_activation_required &&
		!immediate_record->visual_ready && immediate_render_sink.calls == 1,
		"GPU placeholder fast path did not preserve external activation");
	check(service.confirm_external_visual_activation(
			placeholder->key, { 40 }
		) == wt::WtApplicationStatus::StaleGeneration,
		"stale GPU activation was accepted");
	check(service.confirm_external_visual_activation(
			placeholder->key, { 41 }
		) == wt::WtApplicationStatus::InvalidInput,
		"unprepared GPU activation was accepted");
	check(service.confirm_external_visual_prepared(
			placeholder->key, { 41 }, placeholder->transition_mask ^ 1U
		) == wt::WtApplicationStatus::InvalidInput,
		"GPU preparation accepted the wrong transition mask");
	check(service.confirm_external_visual_prepared(
			placeholder->key, { 41 }, placeholder->transition_mask
		) == wt::WtApplicationStatus::Ok,
		"current GPU preparation was rejected");
	record = service.find_record(placeholder->key);
	check(record != nullptr && record->external_visual_prepared &&
		!record->visual_ready && record->staged_replacement,
		"GPU preparation retired coverage before activation");
	check(service.submit_render(placeholder) == wt::WtApplicationStatus::Ok,
		"duplicate prepared GPU placeholder submission failed");
	service.apply(1, 0, render_sink, collision_sink);
	record = service.find_record(placeholder->key);
	check(record != nullptr && record->external_visual_prepared &&
		!record->visual_ready && record->external_visual_activation_required,
		"duplicate current GPU placeholder revoked preparation");
	check(service.confirm_external_visual_activation(
			placeholder->key, { 41 }
		) == wt::WtApplicationStatus::Ok,
		"prepared GPU activation was rejected");
	record = service.find_record(placeholder->key);
	check(record != nullptr && record->visual_ready &&
		!record->external_visual_activation_required &&
		!record->staged_replacement && record->fully_ready(),
		"GPU activation did not release visual readiness");
	check(service.confirm_external_visual_activation(
			placeholder->key, { 41 }
		) == wt::WtApplicationStatus::AlreadyCurrent,
		"repeated GPU activation was not idempotent");
	check(service.submit_render(placeholder) == wt::WtApplicationStatus::Ok,
		"duplicate current GPU placeholder submission failed");
	service.apply(1, 0, render_sink, collision_sink);
	record = service.find_record(placeholder->key);
	check(record != nullptr && record->visual_ready &&
		!record->external_visual_activation_required &&
		record->external_visual_transition_mask == placeholder->transition_mask &&
		record->visual_generation.value == 41 && record->fully_ready(),
		"duplicate current GPU placeholder revoked external activation");
	auto changed_transition = std::make_shared<wt::WtRenderPayload>(*placeholder);
	changed_transition->transition_mask ^=
		wt::wt_face_bit(wt::WtChunkFace::PositiveZ);
	check(service.submit_render(changed_transition) == wt::WtApplicationStatus::Ok,
		"changed-transition GPU placeholder submission failed");
	service.apply(1, 0, render_sink, collision_sink);
	record = service.find_record(placeholder->key);
	check(record != nullptr && !record->visual_ready &&
		record->external_visual_activation_required &&
		!record->external_visual_prepared &&
		record->external_visual_transition_mask == changed_transition->transition_mask &&
		record->visual_generation.value == 41 && !record->fully_ready(),
		"changed GPU transition mask retained stale visual readiness");
}

void test_independent_empty_replacement_collision_precedes_visual(
	const wt::WtRenderPayload &render_source
) {
	wt::WtChunkApplicationService service(1, 1, 1);
	RenderSink render_sink;
	CollisionSink collision_sink;
	auto placeholder = std::make_shared<wt::WtRenderPayload>(render_source);
	placeholder->generation = { 42 };
	placeholder->publication_source =
		wt::WtRenderPublicationSource::GpuResidentPlaceholder;
	auto empty_collision = std::make_shared<wt::WtCollisionPayload>();
	empty_collision->key = placeholder->key;
	empty_collision->generation = placeholder->generation;
	empty_collision->world_origin = wt::wt_chunk_bounds(placeholder->key).minimum;
	check(service.expect_chunk(
			placeholder->key, placeholder->generation,
			true, true, true, false, 0, true
		) == wt::WtApplicationStatus::Ok &&
		service.submit_render(placeholder) == wt::WtApplicationStatus::Ok &&
		service.submit_collision(empty_collision) == wt::WtApplicationStatus::Ok,
		"empty GPU collision setup failed");
	service.apply(0, 1, render_sink, collision_sink);
	const wt::WtChunkApplicationRecord *record =
		service.find_record(placeholder->key);
	check(collision_sink.calls == 1 && service.deferred_collision_count() == 0 &&
		record != nullptr && record->collision_ready &&
		!record->visual_ready && record->staged_replacement,
		"independent empty replacement collision waited for visual activation");
	service.apply(1, 0, render_sink, collision_sink);
	check(service.confirm_external_visual_prepared(
			placeholder->key, placeholder->generation, placeholder->transition_mask
		) == wt::WtApplicationStatus::Ok &&
		service.confirm_external_visual_activation(
			placeholder->key, placeholder->generation
		) == wt::WtApplicationStatus::Ok,
		"empty replacement visual activation failed");
	service.apply(0, 1, render_sink, collision_sink);
	record = service.find_record(placeholder->key);
	check(collision_sink.calls == 1 && service.deferred_collision_count() == 0 &&
		record != nullptr && record->collision_ready &&
		record->collision_generation == placeholder->generation &&
		record->visual_ready && record->fully_ready(),
		"independent empty replacement did not retain collision readiness");
}

void test_superseded_gpu_visual_generation(
	const wt::WtRenderPayload &render_source
) {
	wt::WtChunkApplicationService service(1, 1, 1);
	RenderSink render_sink;
	CollisionSink collision_sink;
	auto placeholder = std::make_shared<wt::WtRenderPayload>(render_source);
	placeholder->generation = { 51 };
	placeholder->publication_source =
		wt::WtRenderPublicationSource::GpuResidentPlaceholder;
	check(service.expect_chunk(
			placeholder->key, { 51 }, false, true, true
		) == wt::WtApplicationStatus::Ok &&
		service.submit_render(placeholder) == wt::WtApplicationStatus::Ok,
		"superseded GPU visual setup failed");
	check(service.supersede_visual_generation(
			placeholder->key, { 50 }
		) == wt::WtApplicationStatus::StaleGeneration &&
		service.supersede_visual_generation(
			placeholder->key, { 51 }
		) == wt::WtApplicationStatus::Ok &&
		service.supersede_visual_generation(
			placeholder->key, { 51 }
		) == wt::WtApplicationStatus::AlreadyCurrent,
		"GPU visual supersession was not exact and idempotent");
	service.apply(1, 0, render_sink, collision_sink);
	const wt::WtChunkApplicationRecord *record =
		service.find_record(placeholder->key);
	check(record != nullptr && record->visual_generation_superseded &&
		!record->visual_ready && record->visual_generation.value == 0 &&
		!record->fully_ready() && render_sink.calls == 0,
		"superseded GPU placeholder reached the render sink");
	check(service.confirm_external_visual_prepared(
			placeholder->key, { 51 }, placeholder->transition_mask
		) == wt::WtApplicationStatus::StaleGeneration,
		"superseded GPU visual was prepared");
	check(service.expect_chunk(
			placeholder->key, { 52 }, false, true, true
		) == wt::WtApplicationStatus::Ok,
		"successor expectation after visual supersession failed");
	record = service.find_record(placeholder->key);
	check(record != nullptr && !record->visual_generation_superseded,
		"successor generation inherited visual supersession");
}

void test_cross_lod_replacement_publication_policy() {
	const wt::WtChunkKey fine { 2, 0, 0, 0 };
	const wt::WtChunkKey containing_coarse { 1, 0, 0, 1 };
	const wt::WtChunkKey adjacent_coarse { 0, 0, 0, 1 };
	const wt::WtChunkKey distant_coarse { 3, 0, 0, 1 };
	const wt::WtChunkKey negative_fine { -1, 0, 0, 0 };
	const wt::WtChunkKey negative_coarse { -1, 0, 0, 1 };
	check(
		!wt::wt_chunk_replacement_requires_regional_publication(fine, {}),
		"replacement without retirements was not independently publishable"
	);
	check(
		wt::wt_chunk_replacement_requires_regional_publication(
			fine,
			{ containing_coarse }
		),
		"fine replacement could publish beneath retained coarse terrain"
	);
	check(
		!wt::wt_chunk_replacement_requires_regional_publication(
			fine,
			{ adjacent_coarse, distant_coarse }
		),
		"touching or distant retirement blocked independent publication"
	);
	check(
		wt::wt_chunk_replacement_requires_regional_publication(
			containing_coarse,
			{ fine }
		),
		"coarse replacement could publish over retained fine terrain"
	);
	check(
		wt::wt_chunk_replacement_requires_regional_publication(
			negative_fine,
			{ negative_coarse }
		),
		"negative-coordinate cross-LOD overlap was not detected"
	);

	const std::vector<wt::WtChunkKey> replacements {
		{ 0, 0, 0, 0 },
		{ 1, 0, 0, 0 },
		{ 8, 0, 0, 0 },
		{ 0, 1, 0, 0 },
		{ 1, 1, 0, 0 },
		{ 0, 0, 1, 0 },
		{ 1, 0, 1, 0 },
		{ 0, 1, 1, 0 },
		{ 1, 1, 1, 0 },
	};
	const std::vector<wt::WtChunkKey> retirements {
		{ 0, 0, 0, 1 },
		{ 4, 0, 0, 1 },
	};
	wt::WtChunkPublicationRegion region;
	check(
		wt::wt_build_chunk_publication_region(
			{ 0, 0, 0, 0 },
			replacements,
			retirements,
			region
		),
		"cross-LOD publication region was not built"
	);
	check(region.replacements.size() == 8 &&
		region.retirements.size() == 1 &&
		std::find(
			region.replacements.begin(),
			region.replacements.end(),
			wt::WtChunkKey { 8, 0, 0, 0 }
		) == region.replacements.end(),
		"publication region did not isolate the connected overlap component");
	check(
		wt::wt_chunk_publication_region_has_complete_coverage(region),
		"complete fine replacement set did not cover its coarse retirement"
	);
	const std::vector<wt::WtChunkKey> complete_collision_coverage =
		region.replacements;
	std::vector<wt::WtChunkKey> partial_collision_coverage =
		complete_collision_coverage;
	partial_collision_coverage.pop_back();
	check(
		!wt::wt_collision_retirement_is_safe(
			region.retirements.front(),
			complete_collision_coverage,
			partial_collision_coverage
		),
		"partial physical collision coverage retired its coarse safety shape"
	);
	check(
		wt::wt_collision_retirement_is_safe(
			region.retirements.front(),
			complete_collision_coverage,
			complete_collision_coverage
		),
		"complete physical collision coverage retained its obsolete coarse shape"
	);
	check(
		wt::wt_collision_retirement_is_safe(
			region.retirements.front(),
			{ { 8, 0, 0, 0 } },
			{}
		),
		"collision shape outside current demand was not safely retired"
	);
	check(
		wt::wt_required_collision_can_publish_independently(
			{ 21 }, { 21 }, {}, { 21 }, true, true, true
		),
		"new required collision did not join its live visual independently"
	);
	check(
		wt::wt_required_collision_can_publish_independently(
			{ 21 }, {}, {}, { 21 }, true, true, false
		),
		"collision-only support was tied to visual publication"
	);
	check(
		!wt::wt_required_collision_can_publish_independently(
			{ 21 }, { 21 }, { 20 }, { 21 }, true, true, true
		) && !wt::wt_required_collision_can_publish_independently(
			{ 21 }, {}, {}, { 21 }, true, true, true
		) && !wt::wt_required_collision_can_publish_independently(
			{ 21 }, { 21 }, {}, { 20 }, true, true, true
		) && !wt::wt_required_collision_can_publish_independently(
			{ 21 }, { 21 }, {}, { 21 }, false, true, true
		),
		"independent collision publication accepted a replacement, hidden "
		"visual, stale generation, or unrequired shape"
	);
	region.replacements.pop_back();
	check(
		!wt::wt_chunk_publication_region_has_complete_coverage(region),
		"partial fine replacement set retired incomplete coarse coverage"
	);
	check(
		!wt::wt_chunk_publication_region_has_complete_coverage({
			{ { 0, 0, 0, 1 }, { 0, 0, 0, 0 } },
			{ { 0, 0, 0, 1 } },
		}),
		"overlapping replacement ownership was accepted as complete coverage"
	);
	check(
		!wt::wt_chunk_publication_region_has_complete_coverage({
			{ { 0, 0, 0, 1 } },
			{ { 0, 0, 0, 1 } },
		}),
		"one chunk was accepted as both replacement and retirement"
	);
	check(
		!wt::wt_build_chunk_publication_region(
			{ 7, 0, 0, 0 },
			replacements,
			retirements,
			region
		),
		"publication region accepted a missing seed"
	);

	std::vector<wt::WtChunkKey> balanced_replacements;
	for (std::int32_t z = 0; z < 4; ++z) {
		for (std::int32_t y = 0; y < 4; ++y) {
			for (std::int32_t x = 0; x < 4; ++x) {
				balanced_replacements.push_back({ x, y, z, 1 });
			}
		}
	}
	for (std::int32_t z = 0; z < 2; ++z) {
		for (std::int32_t y = 0; y < 2; ++y) {
			for (std::int32_t x = 2; x < 4; ++x) {
				balanced_replacements.push_back({ x, y, z, 2 });
			}
		}
	}
	std::sort(balanced_replacements.begin(), balanced_replacements.end());
	const std::vector<wt::WtChunkKey> balanced_retirements {
		{ 0, 0, 0, 3 },
		{ 1, 0, 0, 3 },
	};
	check(
		wt::wt_build_chunk_publication_region(
			{ 3, 0, 0, 1 },
			balanced_replacements,
			balanced_retirements,
			region
		) && region.replacements.size() == 72 &&
		region.retirements.size() == 2 &&
		std::find(
			region.replacements.begin(),
			region.replacements.end(),
			wt::WtChunkKey { 2, 0, 0, 2 }
		) != region.replacements.end() &&
		wt::wt_chunk_publication_region_has_complete_coverage(region),
		"publication region exposed an LOD1-to-LOD3 boundary without its LOD2 bridge"
	);

	std::vector<wt::WtChunkKey> finite_replacements;
	for (std::int32_t z = 0; z < 4; ++z) {
		for (std::int32_t y = 0; y < 4; ++y) {
			for (std::int32_t x = 0; x < 4; ++x) {
				finite_replacements.push_back({ x, y, z, 0 });
			}
		}
	}
	for (std::int32_t z = 0; z < 2; ++z) {
		for (std::int32_t y = 0; y < 2; ++y) {
			for (std::int32_t x = 2; x < 4; ++x) {
				finite_replacements.push_back({ x, y, z, 1 });
			}
		}
	}
	finite_replacements.push_back({ 2, 0, 0, 2 });
	std::sort(finite_replacements.begin(), finite_replacements.end());
	const std::vector<wt::WtChunkKey> finite_retirements {
		{ 0, 0, 0, 3 },
		{ 1, 0, 0, 3 },
	};
	check(
		wt::wt_build_chunk_publication_region(
			{ 0, 0, 0, 0 },
			finite_replacements,
			finite_retirements,
			region
		) && region.replacements.size() == 73 &&
			region.retirements.size() == 2 &&
			!wt::wt_chunk_publication_region_has_complete_coverage(region),
		"finite-world fixture unexpectedly covered the unbounded retirement cubes"
	);
	const auto finite_authority = [](const wt::WtChunkKey &key) {
		if (!wt::wt_is_valid_chunk_key(key)) return false;
		return key.x >= 0 && key.y >= 0 && key.z >= 0 &&
			key.x <= ((12 - 1) >> key.lod) &&
			key.y <= ((4 - 1) >> key.lod) &&
			key.z <= ((4 - 1) >> key.lod);
	};
	check(
		wt::wt_chunk_publication_region_has_complete_authoritative_coverage(
			region, finite_authority
		),
		"complete finite-world authority coverage did not replace clipped coarse roots"
	);
	region.replacements.erase(std::find(
		region.replacements.begin(),
		region.replacements.end(),
		wt::WtChunkKey { 3, 3, 3, 0 }
	));
	check(
		!wt::wt_chunk_publication_region_has_complete_authoritative_coverage(
			region, finite_authority
		),
		"finite-world authority coverage accepted a missing declared child"
	);

	std::vector<wt::WtChunkKey> backlog_replacements = replacements;
	std::vector<wt::WtChunkKey> backlog_retirements = retirements;
	for (std::int32_t index = 0; index < 2048; ++index) {
		backlog_replacements.push_back({ 10000 + index, 0, 0, 0 });
		backlog_retirements.push_back({ 10000 + index, 8, 0, 0 });
	}
	std::sort(backlog_replacements.begin(), backlog_replacements.end());
	std::sort(backlog_retirements.begin(), backlog_retirements.end());
	check(
		wt::wt_build_chunk_publication_region(
			{ 0, 0, 0, 0 },
			backlog_replacements,
			backlog_retirements,
			region
		) && region.replacements.size() == 8 &&
			region.retirements.size() == 1 &&
			wt::wt_chunk_publication_region_has_complete_coverage(region),
		"large unrelated backlog contaminated the publication region"
	);
}

void test_gpu_reciprocal_publication_dependencies() {
	for (const wt::WtChunkKey coarse : { wt::WtChunkKey { 9, 0, 6, 3 },
			wt::WtChunkKey { -2, -1, -2, 1 } }) {
		for (std::uint8_t face_index = 0; face_index < 6; ++face_index) {
			const auto face = static_cast<wt::WtChunkFace>(face_index);
			const auto bit = wt::wt_face_bit(face);
			const auto axis = face_index / 2;
			const auto sign = (face_index & 1) == 0 ? -1 : 1;
			std::int32_t coordinate[3] { coarse.x, coarse.y, coarse.z };
			coordinate[axis] += sign;
			const wt::WtChunkKey replaced { coordinate[0], coordinate[1], coordinate[2], coarse.lod };
			std::vector<wt::WtChunkKey> fine;
			for (std::int32_t z = 0; z < 2; ++z) {
				for (std::int32_t y = 0; y < 2; ++y) {
					for (std::int32_t x = 0; x < 2; ++x) {
						fine.push_back({ replaced.x * 2 + x, replaced.y * 2 + y,
							replaced.z * 2 + z, static_cast<std::uint8_t>(coarse.lod - 1) });
					}
				}
			}
			std::sort(fine.begin(), fine.end());
			wt::WtChunkPublicationRegion legacy;
			check(wt::wt_build_chunk_publication_region(fine.front(), fine, { replaced }, legacy) &&
				std::find(legacy.replacements.begin(), legacy.replacements.end(), coarse) == legacy.replacements.end(),
				"legacy overlap-only control unexpectedly included the transition neighbor");
			std::uint8_t coarse_mask = 0;
			bool coarse_compatible = false;
			bool coarse_mask_known = true;
			bool refining = true;
			const auto lookup = [&](const wt::WtChunkKey &key, wt::WtGpuPublicationBoundary &state) {
				if (key == coarse) {
					state = wt::wt_gpu_publication_boundary(
						coarse_mask, coarse_mask_known, bit, coarse_compatible
					);
					return true;
				}
				if ((refining && std::binary_search(fine.begin(), fine.end(), key)) ||
					(!refining && key == replaced)) {
					state = { 0, false };
					return true;
				}
				return false;
			};
			std::vector<wt::WtChunkKey> candidates = fine;
			candidates.push_back(coarse);
			candidates.push_back({ 10000, 0, 0, 0 });
			std::sort(candidates.begin(), candidates.end());
			wt::WtChunkPublicationRegion region;
			std::vector<wt::WtChunkKey> waiting;
			check(wt::wt_build_gpu_chunk_publication_cohort(
					fine.front(), candidates, { replaced }, lookup, region, waiting) &&
				region.replacements.size() == 9 && region.retirements == std::vector<wt::WtChunkKey>{replaced} &&
				waiting == std::vector<wt::WtChunkKey>{coarse},
				"fine replacement did not wait for reciprocal coarse transition");
			coarse_mask = bit;
			check(wt::wt_build_gpu_chunk_publication_cohort(
					fine.front(), candidates, { replaced }, lookup, region, waiting) &&
				waiting.empty() && region.replacements.size() == 9 &&
				wt::wt_chunk_publication_region_has_complete_coverage(region),
				"refinement did not select both sides and their complete overlap region");
			coarse_compatible = true;
			check(wt::wt_build_gpu_chunk_publication_cohort(
					fine.front(), candidates, { replaced }, lookup, region, waiting) &&
				waiting.empty() && region.replacements == fine &&
				region.retirements == std::vector<wt::WtChunkKey>{replaced},
				"compatible active transition neighbor was needlessly republished");
			// Expecting a successor resets the application mask before its mesh
			// exists. The retained, already active transition still covers this face.
			coarse_mask = 0;
			coarse_mask_known = false;
			check(wt::wt_build_gpu_chunk_publication_cohort(
					fine.front(), candidates, { replaced }, lookup, region, waiting) &&
				waiting.empty() && region.replacements == fine &&
				wt::wt_chunk_publication_region_has_complete_coverage(region),
				"unknown successor mask hid a compatible live boundary");
			coarse_mask_known = true;
			check(wt::wt_build_gpu_chunk_publication_cohort(
					fine.front(), candidates, { replaced }, lookup, region, waiting) &&
				waiting == std::vector<wt::WtChunkKey>{coarse},
				"known incompatible successor mask bypassed reciprocal validation");
			coarse_mask = bit;
			coarse_compatible = false;
			check(!wt::wt_build_gpu_chunk_publication_cohort(
					fine.front(), candidates, { replaced }, lookup, region, waiting, 8),
				"bounded GPU cohort silently truncated a required neighbor");
			refining = false;
			candidates = {coarse, replaced};
			std::sort(candidates.begin(), candidates.end());
			check(wt::wt_build_gpu_chunk_publication_cohort(
					replaced, candidates, fine, lookup, region, waiting) &&
				region.replacements == candidates && region.retirements == fine &&
				waiting == std::vector<wt::WtChunkKey>{coarse},
				"coarsening did not wait for obsolete transition removal");
			coarse_mask = 0;
			// The successor's zero mask is not the mask currently on screen.
			coarse_compatible = true;
			check(wt::wt_build_gpu_chunk_publication_cohort(
					replaced, candidates, fine, lookup, region, waiting) &&
				waiting.empty() && region.replacements == candidates &&
				region.retirements == fine &&
				wt::wt_chunk_publication_region_has_complete_coverage(region),
				"coarsening left the retained neighbor's obsolete transition on screen");
		}
	}
}

void test_gpu_edit_content_boundaries() {
	// A brush spanning this face must never show only one edited half, even
	// while the neighbor has only an expectation and no candidate mask yet.
	const wt::WtChunkKey left { -1, 0, 0, 0 };
	const wt::WtChunkKey right { 0, 0, 0, 0 };
	const wt::WtChunkKey unchanged { 1, 0, 0, 0 };
	for (const bool candidate_known : { false, true }) {
		const auto lookup = [&](const wt::WtChunkKey &key, wt::WtGpuPublicationBoundary &state) {
			if (key != left && key != right && key != unchanged) return false;
			state = wt::wt_gpu_publication_boundary(0, candidate_known, 0, true, key == unchanged);
			return true;
		};
		wt::WtChunkPublicationRegion region;
		std::vector<wt::WtChunkKey> waiting;
		check(wt::wt_build_gpu_chunk_publication_cohort(
			left, {left, right}, {}, lookup, region, waiting) &&
			region.replacements == std::vector<wt::WtChunkKey> { left, right } && waiting.empty(),
			"matching masks published only half of a cross-chunk edit");
		check(wt::wt_build_gpu_chunk_publication_cohort(
			right, {left, right}, {}, lookup, region, waiting) &&
			region.replacements == std::vector<wt::WtChunkKey> { left, right },
			"edit cohort depended on which edited half became ready first");
	}
}

void test_gpu_publication_dependency_bounds() {
	const wt::WtChunkKey seed { -1, 0, 0, 0 };
	const wt::WtChunkKey retained { 0, 0, 0, 1 };
	const wt::WtChunkKey unrelated { 1, 0, 0, 1 };
	bool invalid_retained_fine = false;
	const auto lookup = [&](const wt::WtChunkKey &key, wt::WtGpuPublicationBoundary &state) {
		if (key == retained) {
			state = { wt::wt_face_bit(wt::WtChunkFace::NegativeX), !invalid_retained_fine };
			return true;
		}
		if (key == unrelated || key == seed) {
			state = { static_cast<std::uint8_t>(invalid_retained_fine && key == seed ?
				wt::wt_face_bit(wt::WtChunkFace::PositiveX) : 0), invalid_retained_fine };
			return true;
		}
		if (key.lod == 0 && key.x == -1 && key.y >= 0 && key.y < 2 && key.z >= 0 && key.z < 2) {
			state = { 0, true };
			return true;
		}
		return false;
	};
	const wt::WtChunkKey obsolete_under_retained { 0, 0, 0, 0 };
	std::vector<wt::WtChunkKey> pending { seed, unrelated };
	std::sort(pending.begin(), pending.end());
	wt::WtChunkPublicationRegion region;
	std::vector<wt::WtChunkKey> waiting;
	const std::vector<wt::WtChunkKey> expected { seed };
	check(wt::wt_build_gpu_chunk_publication_cohort(
			seed, pending, {obsolete_under_retained}, lookup, region, waiting) &&
		region.replacements == expected && waiting.empty(),
		"unchanged reciprocal neighbor expanded its unrelated overlap region");
	invalid_retained_fine = true;
	check(wt::wt_build_gpu_chunk_publication_cohort(retained, pending, {}, lookup, region, waiting) &&
		std::binary_search(waiting.begin(), waiting.end(), seed),
		"retained finer neighbor bypassed reciprocal transition-mask validation");
	check(!wt::wt_build_gpu_chunk_publication_cohort(seed, pending, {seed}, lookup, region, waiting),
		"retiring GPU seed was accepted as desired geometry");
	check(!wt::wt_build_gpu_chunk_publication_cohort({4, 4, 4, 0}, pending, {}, lookup, region, waiting),
		"missing GPU seed was accepted as desired geometry");
	const wt::WtChunkKey limit { std::numeric_limits<std::int32_t>::max(), 0, 0, 1 };
	bool transition_required = false;
	const auto limit_lookup = [&](const wt::WtChunkKey &key, wt::WtGpuPublicationBoundary &state) {
		if (key != limit) return false;
		state = { static_cast<std::uint8_t>(transition_required ?
			wt::wt_face_bit(wt::WtChunkFace::PositiveX) : 0), false };
		return true;
	};
	check(wt::wt_build_gpu_chunk_publication_cohort(limit, {limit}, {}, limit_lookup, region, waiting),
		"absent finer keys outside representable range rejected an independent chunk");
	transition_required = true;
	check(!wt::wt_build_gpu_chunk_publication_cohort(limit, {limit}, {}, limit_lookup, region, waiting),
		"unrepresentable required finer keys silently removed a transition dependency");
}

void test_gpu_same_lod_candidates_preserve_retained_boundaries() {
	const wt::WtChunkKey left_parent { 0, 0, 0, 1 };
	const wt::WtChunkKey right_parent { 1, 0, 0, 1 };
	std::vector<wt::WtChunkKey> left_children;
	std::vector<wt::WtChunkKey> right_children;
	for (std::int32_t z = 0; z < 2; ++z) {
		for (std::int32_t y = 0; y < 2; ++y) {
			for (std::int32_t x = 0; x < 2; ++x) {
				left_children.push_back({ x, y, z, 0 });
				right_children.push_back({ 2 + x, y, z, 0 });
			}
		}
	}
	std::vector<wt::WtChunkKey> candidates = left_children;
	candidates.insert(
		candidates.end(), right_children.begin(), right_children.end()
	);
	std::sort(candidates.begin(), candidates.end());
	const auto lookup = [&candidates](
			const wt::WtChunkKey &key,
			wt::WtGpuPublicationBoundary &boundary
		) {
		if (!std::binary_search(candidates.begin(), candidates.end(), key)) {
			return false;
		}
		boundary = { 0, false };
		return true;
	};
	wt::WtChunkPublicationRegion region;
	std::vector<wt::WtChunkKey> waiting;
	check(
		wt::wt_build_gpu_chunk_publication_cohort(
			left_children.front(),
			candidates,
			{ left_parent, right_parent },
			lookup,
			region,
			waiting
		) && region.replacements == candidates &&
			region.retirements == std::vector<wt::WtChunkKey> { left_parent, right_parent } &&
			waiting.empty() &&
			wt::wt_chunk_publication_region_has_complete_coverage(region),
		"same-LOD candidates exposed an unstitched retained coarse neighbor"
	);
}

void test_collision_deadline_bounds_frame_work(
	const wt::WtRenderPayload &render_source
) {
	wt::WtChunkApplicationService service(3, 1, 3);
	RenderSink render_sink;
	SlowCollisionSink collision_sink;
	for (std::int32_t x = 0; x < 3; ++x) {
		wt::WtRenderPayload render = render_source;
		render.key = { x, 0, 0, 0 };
		render.world_origin = wt::wt_chunk_bounds(render.key).minimum;
		render.generation = { 1 };
		auto collision = std::make_shared<wt::WtCollisionPayload>();
		check(wt::wt_build_collision_payload(render, {}, *collision) ==
			wt::WtCollisionBuildStatus::Ok,
			"deadline collision payload build failed");
		check(service.expect_chunk(render.key, { 1 }, true, false) ==
			wt::WtApplicationStatus::Ok,
			"deadline collision expectation failed");
		check(service.submit_collision(collision) == wt::WtApplicationStatus::Ok,
			"deadline collision submission failed");
	}
	const wt::WtApplicationBatchResult bounded =
		service.apply_with_collision_deadline(
			0,
			3,
			1000000,
			render_sink,
			collision_sink
		);
	check(bounded.collision_processed == 1 &&
		bounded.collision_deadline_exhausted &&
		collision_sink.calls == 1,
		"collision deadline did not bound frame work");
	check(service.queued_collision_count() == 1 &&
		service.deferred_collision_count() == 1,
		"deadline leftovers were not retained as backlog");
	const wt::WtApplicationMetrics deadline_metrics = service.get_metrics();
	check(deadline_metrics.collision_apply_deadline_exhaustions == 1 &&
		deadline_metrics.collision_apply_time_ns_last >= 1000000,
		"collision deadline metrics were not recorded");
	const wt::WtApplicationBatchResult drained =
		service.apply_with_collision_deadline(
			0,
			3,
			0,
			render_sink,
			collision_sink
		);
	check(drained.collision_processed == 2 &&
		!drained.collision_deadline_exhausted &&
		collision_sink.calls == 3 &&
		service.queued_collision_count() == 0 &&
		service.deferred_collision_count() == 0,
		"deadline collision backlog did not drain later");
}

} // namespace

int main() {
	wt::WtRenderPayload render;
	test_render_builder(render);
	test_collision_builder();
	test_interaction_collision_apply_priority();
	constexpr std::size_t stale_cycles = 1000;
	test_application_service(render, stale_cycles);
	test_collision_revision_is_independent_from_visual_generation(render);
	test_staged_replacement_collision_waits_for_render(render);
	test_gpu_placeholder_waits_for_external_activation(render);
	test_superseded_gpu_visual_generation(render);
	test_independent_empty_replacement_collision_precedes_visual(render);
	test_cross_lod_replacement_publication_policy();
	test_gpu_reciprocal_publication_dependencies();
	test_gpu_publication_dependency_bounds();
	test_gpu_edit_content_boundaries();
	test_gpu_same_lod_candidates_preserve_retained_boundaries();
	test_collision_deadline_bounds_frame_work(render);
	if (failure_count != 0) {
		std::fprintf(stderr, "M3_APPLICATION_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf(
		"M3_APPLICATION_PASS stale_cycles=%zu cross_lod_publication=1 reciprocal_gpu_faces=12\n",
		stale_cycles
	);
	return 0;
}
