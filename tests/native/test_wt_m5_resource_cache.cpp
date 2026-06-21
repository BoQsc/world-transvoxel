#include "services/wt_chunk_resource_cache.h"
#include "storage/wt_hash256.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

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
	float offset,
	std::uint16_t material
) {
	buffer.vertices = {
		vertex(offset, 0.0F, 0.0F, material),
		vertex(offset + 1.0F, 0.0F, 0.0F, material),
		vertex(offset, 0.0F, 1.0F, material),
	};
	buffer.indices = { 0, 1, 2 };
}

std::shared_ptr<const wt::WtChunkMeshResult> make_mesh(
	const wt::WtChunkKey &key,
	float offset,
	std::uint16_t material
) {
	auto mesh = std::make_shared<wt::WtChunkMeshResult>();
	mesh->key = key;
	mesh->world_origin = wt::wt_chunk_bounds(key).minimum;
	add_triangle(mesh->regular, offset, material);
	return mesh;
}

std::shared_ptr<const wt::WtRenderPayload> make_render(
	const wt::WtChunkMeshResult &mesh,
	std::uint64_t generation
) {
	auto render = std::make_shared<wt::WtRenderPayload>();
	check(
		wt::wt_build_render_payload(mesh, { generation }, *render) ==
			wt::WtRenderBuildStatus::Ok,
		"render fixture build failed"
	);
	return render;
}

std::shared_ptr<const wt::WtCollisionPayload> make_collision(
	const wt::WtRenderPayload &render
) {
	auto collision = std::make_shared<wt::WtCollisionPayload>();
	check(
		wt::wt_build_collision_payload(render, {}, *collision) ==
			wt::WtCollisionBuildStatus::Ok,
		"collision fixture build failed"
	);
	return collision;
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
	for (std::size_t index = 0; index < 8; ++index) {
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
	}
}

void append_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
	for (std::size_t index = 0; index < 4; ++index) {
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
	}
}

void append_f32(std::vector<std::uint8_t> &bytes, float value) {
	std::uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&bits, &value, sizeof(bits));
	append_u32(bytes, bits);
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void test_invalid_and_oversize(
	const std::shared_ptr<const wt::WtChunkMeshResult> &mesh,
	const std::shared_ptr<const wt::WtRenderPayload> &render,
	const std::shared_ptr<const wt::WtCollisionPayload> &collision
) {
	wt::WtChunkResourceCache invalid({
		0, wt::kWtMaximumResourceCacheBytes,
		1, wt::kWtMaximumResourceCacheBytes,
		1, wt::kWtMaximumResourceCacheBytes,
	});
	check(!invalid.valid(), "zero mesh capacity was accepted");
	check(
		invalid.insert_mesh(mesh, { 1 }, { 1 }) ==
			wt::WtChunkResourceCacheStatus::InvalidConfiguration,
		"invalid cache accepted a mesh"
	);

	wt::WtChunkResourceCache small({
		1, sizeof(wt::WtChunkMeshResult),
		1, sizeof(wt::WtRenderPayload),
		1, sizeof(wt::WtCollisionPayload),
	});
	check(
		small.insert_mesh(mesh, { 1 }, { 1 }) ==
			wt::WtChunkResourceCacheStatus::MeshItemTooLarge &&
		small.insert_render(render, render->generation) ==
			wt::WtChunkResourceCacheStatus::RenderItemTooLarge &&
		small.insert_collision(collision, collision->generation) ==
			wt::WtChunkResourceCacheStatus::CollisionItemTooLarge,
		"oversize derived payload was accepted"
	);
}

void test_rejections(
	wt::WtChunkResourceCache &cache,
	const std::shared_ptr<const wt::WtChunkMeshResult> &mesh,
	const std::shared_ptr<const wt::WtRenderPayload> &render,
	const std::shared_ptr<const wt::WtCollisionPayload> &collision
) {
	check(
		cache.insert_mesh(mesh, { 1 }, { 2 }) ==
			wt::WtChunkResourceCacheStatus::StaleGeneration &&
		cache.insert_render(render, { render->generation.value + 1 }) ==
			wt::WtChunkResourceCacheStatus::StaleGeneration &&
		cache.insert_collision(
			collision,
			{ collision->generation.value + 1 }
		) == wt::WtChunkResourceCacheStatus::StaleGeneration,
		"stale derived payload was accepted"
	);

	auto invalid_mesh = std::make_shared<wt::WtChunkMeshResult>(*mesh);
	invalid_mesh->regular.indices[2] = 99;
	auto invalid_render = std::make_shared<wt::WtRenderPayload>(*render);
	invalid_render->vertices[0].position.x =
		std::numeric_limits<float>::quiet_NaN();
	auto invalid_collision =
		std::make_shared<wt::WtCollisionPayload>(*collision);
	++invalid_collision->metrics.output_triangles;
	check(
		cache.insert_mesh(invalid_mesh, { 3 }, { 3 }) ==
			wt::WtChunkResourceCacheStatus::InvalidPayload &&
		cache.insert_render(invalid_render, invalid_render->generation) ==
			wt::WtChunkResourceCacheStatus::InvalidPayload &&
		cache.insert_collision(
			invalid_collision,
			invalid_collision->generation
		) == wt::WtChunkResourceCacheStatus::InvalidPayload,
		"invalid derived payload was accepted"
	);
}

void test_mesh_tier(
	wt::WtChunkResourceCache &cache,
	const std::vector<std::shared_ptr<const wt::WtChunkMeshResult>> &meshes
) {
	check(
		cache.insert_mesh(meshes[0], { 10 }, { 10 }) ==
			wt::WtChunkResourceCacheStatus::Ok &&
		cache.insert_mesh(meshes[1], { 11 }, { 11 }) ==
			wt::WtChunkResourceCacheStatus::Ok,
		"initial mesh cache insertion failed"
	);
	auto mesh0_copy = std::make_shared<wt::WtChunkMeshResult>(*meshes[0]);
	check(
		cache.insert_mesh(mesh0_copy, { 10 }, { 10 }) ==
			wt::WtChunkResourceCacheStatus::Ok,
		"equal mesh refresh failed"
	);
	const auto held_mesh1 = cache.find_mesh(meshes[1]->key, { 11 });
	check(
		held_mesh1 && cache.find_mesh(meshes[0]->key, { 10 }),
		"mesh cache hit failed"
	);
	check(
		cache.insert_mesh(meshes[2], { 12 }, { 12 }) ==
			wt::WtChunkResourceCacheStatus::Ok &&
		!cache.find_mesh(meshes[1]->key, { 11 }) &&
		held_mesh1->regular.vertices.size() == 3,
		"mesh LRU eviction or external ownership failed"
	);
	auto conflict = std::make_shared<wt::WtChunkMeshResult>(*meshes[0]);
	conflict->regular.vertices[0].material += 1;
	check(
		cache.insert_mesh(conflict, { 10 }, { 10 }) ==
			wt::WtChunkResourceCacheStatus::IdentityConflict,
		"conflicting mesh identity was accepted"
	);
	check(
		cache.insert_mesh(meshes[0], { 20 }, { 20 }) ==
			wt::WtChunkResourceCacheStatus::Ok &&
		!cache.find_mesh(meshes[0]->key, { 10 }) &&
		cache.find_mesh(meshes[0]->key, { 20 }),
		"mesh generation supersession failed"
	);
}

void test_render_tier(
	wt::WtChunkResourceCache &cache,
	const std::vector<std::shared_ptr<const wt::WtRenderPayload>> &renders
) {
	check(
		cache.insert_render(renders[0], { 30 }) ==
			wt::WtChunkResourceCacheStatus::Ok &&
		cache.insert_render(renders[1], { 31 }) ==
			wt::WtChunkResourceCacheStatus::Ok,
		"initial render cache insertion failed"
	);
	auto render0_copy = std::make_shared<wt::WtRenderPayload>(*renders[0]);
	check(
		cache.insert_render(render0_copy, { 30 }) ==
			wt::WtChunkResourceCacheStatus::Ok,
		"equal render refresh failed"
	);
	const auto held_render1 = cache.find_render(renders[1]->key, { 31 });
	check(
		held_render1 && cache.find_render(renders[0]->key, { 30 }),
		"render cache hit failed"
	);
	check(
		cache.insert_render(renders[2], { 32 }) ==
			wt::WtChunkResourceCacheStatus::Ok &&
		!cache.find_render(renders[1]->key, { 31 }) &&
		held_render1->vertices.size() == 3,
		"render LRU eviction or external ownership failed"
	);
	auto conflict = std::make_shared<wt::WtRenderPayload>(*renders[0]);
	conflict->vertices[0].material += 1;
	check(
		cache.insert_render(conflict, { 30 }) ==
			wt::WtChunkResourceCacheStatus::IdentityConflict,
		"conflicting render identity was accepted"
	);
	auto superseding = std::make_shared<wt::WtRenderPayload>(*renders[0]);
	superseding->generation = { 40 };
	check(
		cache.insert_render(superseding, { 40 }) ==
			wt::WtChunkResourceCacheStatus::Ok &&
		!cache.find_render(renders[0]->key, { 30 }) &&
		cache.find_render(renders[0]->key, { 40 }),
		"render generation supersession failed"
	);
}

void test_collision_tier(
	wt::WtChunkResourceCache &cache,
	const std::vector<std::shared_ptr<const wt::WtCollisionPayload>> &collisions
) {
	check(
		cache.insert_collision(collisions[0], { 50 }) ==
			wt::WtChunkResourceCacheStatus::Ok &&
		cache.insert_collision(collisions[1], { 51 }) ==
			wt::WtChunkResourceCacheStatus::Ok,
		"initial collision cache insertion failed"
	);
	auto collision0_copy =
		std::make_shared<wt::WtCollisionPayload>(*collisions[0]);
	check(
		cache.insert_collision(collision0_copy, { 50 }) ==
			wt::WtChunkResourceCacheStatus::Ok,
		"equal collision refresh failed"
	);
	const auto held_collision1 =
		cache.find_collision(collisions[1]->key, { 51 });
	check(
		held_collision1 &&
			cache.find_collision(collisions[0]->key, { 50 }),
		"collision cache hit failed"
	);
	check(
		cache.insert_collision(collisions[2], { 52 }) ==
			wt::WtChunkResourceCacheStatus::Ok &&
		!cache.find_collision(collisions[1]->key, { 51 }) &&
		held_collision1->faces.size() == 3,
		"collision LRU eviction or external ownership failed"
	);
	auto conflict =
		std::make_shared<wt::WtCollisionPayload>(*collisions[0]);
	conflict->faces[0].x += 0.25F;
	check(
		cache.insert_collision(conflict, { 50 }) ==
			wt::WtChunkResourceCacheStatus::IdentityConflict,
		"conflicting collision identity was accepted"
	);
	auto superseding =
		std::make_shared<wt::WtCollisionPayload>(*collisions[0]);
	superseding->generation = { 60 };
	check(
		cache.insert_collision(superseding, { 60 }) ==
			wt::WtChunkResourceCacheStatus::Ok &&
		!cache.find_collision(collisions[0]->key, { 50 }) &&
		cache.find_collision(collisions[0]->key, { 60 }),
		"collision generation supersession failed"
	);
}

} // namespace

int main() {
	const std::vector<wt::WtChunkKey> keys = {
		{ 0, 0, 0, 0 },
		{ 1, 0, 0, 0 },
		{ 2, 0, 0, 0 },
	};
	const std::vector<std::shared_ptr<const wt::WtChunkMeshResult>> meshes = {
		make_mesh(keys[0], 0.0F, 1),
		make_mesh(keys[1], 2.0F, 2),
		make_mesh(keys[2], 4.0F, 3),
	};
	const std::vector<std::shared_ptr<const wt::WtRenderPayload>> renders = {
		make_render(*meshes[0], 30),
		make_render(*meshes[1], 31),
		make_render(*meshes[2], 32),
	};
	const std::vector<std::shared_ptr<const wt::WtRenderPayload>>
		collision_renders = {
			make_render(*meshes[0], 50),
			make_render(*meshes[1], 51),
			make_render(*meshes[2], 52),
		};
	const std::vector<std::shared_ptr<const wt::WtCollisionPayload>> collisions = {
		make_collision(*collision_renders[0]),
		make_collision(*collision_renders[1]),
		make_collision(*collision_renders[2]),
	};
	test_invalid_and_oversize(meshes[0], renders[0], collisions[0]);

	wt::WtChunkResourceCache cache({
		2, wt::kWtMaximumResourceCacheBytes,
		2, wt::kWtMaximumResourceCacheBytes,
		2, wt::kWtMaximumResourceCacheBytes,
	});
	check(cache.valid(), "valid resource cache configuration was rejected");
	test_rejections(cache, meshes[0], renders[0], collisions[0]);
	test_mesh_tier(cache, meshes);
	test_render_tier(cache, renders);
	test_collision_tier(cache, collisions);

	check(
		cache.erase_key(keys[0]) == 3 &&
			cache.mesh_entry_count() == 1 &&
			cache.render_entry_count() == 1 &&
			cache.collision_entry_count() == 1,
		"cross-tier key eviction failed"
	);
	const wt::WtChunkResourceCacheMetrics metrics = cache.get_metrics();
	check(
		metrics.stale_rejections == 3 &&
			metrics.invalid_payloads == 3 &&
			metrics.identity_conflicts == 3,
		"resource rejection metrics mismatch"
	);
	check(
		metrics.mesh.insertions == 4 &&
			metrics.mesh.refreshes == 1 &&
			metrics.mesh.evictions == 1 &&
			metrics.mesh.superseded == 1,
		"mesh cache metrics mismatch"
	);
	check(
		metrics.render.insertions == 4 &&
			metrics.render.refreshes == 1 &&
			metrics.render.evictions == 1 &&
			metrics.render.superseded == 1,
		"render cache metrics mismatch"
	);
	check(
		metrics.collision.insertions == 4 &&
			metrics.collision.refreshes == 1 &&
			metrics.collision.evictions == 1 &&
			metrics.collision.superseded == 1,
		"collision cache metrics mismatch"
	);

	std::vector<std::uint8_t> evidence;
	for (const wt::WtCellVertex &item : meshes[0]->regular.vertices) {
		append_f32(evidence, item.position.x);
		append_f32(evidence, item.position.y);
		append_f32(evidence, item.position.z);
		append_f32(evidence, item.normal.x);
		append_f32(evidence, item.normal.y);
		append_f32(evidence, item.normal.z);
		append_u32(evidence, item.material);
		evidence.push_back(item.endpoint_a);
		evidence.push_back(item.endpoint_b);
	}
	append_u64(evidence, metrics.stale_rejections);
	append_u64(evidence, metrics.invalid_payloads);
	append_u64(evidence, metrics.identity_conflicts);
	append_u64(evidence, metrics.mesh.hits);
	append_u64(evidence, metrics.mesh.misses);
	append_u64(evidence, metrics.render.hits);
	append_u64(evidence, metrics.render.misses);
	append_u64(evidence, metrics.collision.hits);
	append_u64(evidence, metrics.collision.misses);
	append_u64(evidence, cache.mesh_resident_bytes());
	append_u64(evidence, cache.render_resident_bytes());
	append_u64(evidence, cache.collision_resident_bytes());

	cache.clear();
	check(
		cache.mesh_entry_count() == 0 &&
			cache.render_entry_count() == 0 &&
			cache.collision_entry_count() == 0 &&
			cache.mesh_resident_bytes() == 0 &&
			cache.render_resident_bytes() == 0 &&
			cache.collision_resident_bytes() == 0,
		"resource cache clear retained residency"
	);
	if (failure_count != 0) {
		std::fprintf(stderr, "M5_RESOURCE_CACHE_FAIL failures=%d\n",
			failure_count);
		return 1;
	}
	std::printf("M5_RESOURCE_CACHE_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"M5_RESOURCE_CACHE_PASS mesh=2 render=2 collision=2 "
		"stale=3 conflicts=3\n"
	);
	return 0;
}
