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
		{ { 6.0F, 0.0F, 4.0F }, { 0.0F, 1.0F, 0.0F }, 9, 0, 0 },
		{ { 6.0F, 1.0F, 4.0F }, { 0.0F, 1.0F, 0.0F }, 9, 0, 0 },
		{ { 6.0F, 0.0F, 5.0F }, { 0.0F, 1.0F, 0.0F }, 9, 0, 0 }
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
		render.water_vertices.size() == 12 && render.water_indices.size() == 9,
		"water heightfield did not reject vertical closure geometry");
	check(render.water_vertices[0].material == 9,
		"water render surface lost its material identity");
	check(render.water_vertices[0].normal.y == 1.0F,
		"water free surface did not receive the authoritative gravity normal");
	check(render.water_vertices[7].position.y == 1.0F &&
		render.water_vertices[10].position.y == 4.0F,
		"water shoreline coverage was not flattened to its gravity level");
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
	check(service.expect_chunk(key, { 3 }, true, true, true, true) ==
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
	constexpr std::size_t stale_cycles = 1000;
	test_application_service(render, stale_cycles);
	test_staged_replacement_collision_waits_for_render(render);
	test_cross_lod_replacement_publication_policy();
	test_collision_deadline_bounds_frame_work(render);
	if (failure_count != 0) {
		std::fprintf(stderr, "M3_APPLICATION_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf(
		"M3_APPLICATION_PASS stale_cycles=%zu cross_lod_publication=1\n",
		stale_cycles
	);
	return 0;
}
