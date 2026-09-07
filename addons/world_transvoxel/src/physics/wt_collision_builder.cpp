#include "physics/wt_collision_builder.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace world_transvoxel {
namespace {

bool is_finite(const WtVec3 &value) noexcept {
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

double distance_squared(const WtVec3 &a, const WtVec3 &b) noexcept {
	const double x = static_cast<double>(b.x) - a.x;
	const double y = static_cast<double>(b.y) - a.y;
	const double z = static_cast<double>(b.z) - a.z;
	return x * x + y * y + z * z;
}

double cross_squared(const WtVec3 &a, const WtVec3 &b, const WtVec3 &c) noexcept {
	const double ab_x = static_cast<double>(b.x) - a.x;
	const double ab_y = static_cast<double>(b.y) - a.y;
	const double ab_z = static_cast<double>(b.z) - a.z;
	const double ac_x = static_cast<double>(c.x) - a.x;
	const double ac_y = static_cast<double>(c.y) - a.y;
	const double ac_z = static_cast<double>(c.z) - a.z;
	const double x = ab_y * ac_z - ab_z * ac_y;
	const double y = ab_z * ac_x - ab_x * ac_z;
	const double z = ab_x * ac_y - ab_y * ac_x;
	return x * x + y * y + z * z;
}

struct CollisionTriangleCandidate {
	WtVec3 a;
	WtVec3 b;
	WtVec3 c;
	double area_squared = 0.0;
	std::size_t order = 0;
	std::uint8_t block = 0;
};

std::uint8_t collision_block_for_triangle(
	const WtVec3 &a,
	const WtVec3 &b,
	const WtVec3 &c,
	std::uint8_t lod
) noexcept {
	const double cell_scale = static_cast<double>(std::uint64_t{1} << lod);
	const auto axis = [cell_scale](float av, float bv, float cv) {
		const double cell = (static_cast<double>(av) + bv + cv) /
			(3.0 * cell_scale);
		return std::clamp(static_cast<int>(std::floor(cell / 8.0)), 0, 1);
	};
	const int x = axis(a.x, b.x, c.x);
	const int y = axis(a.y, b.y, c.y);
	const int z = axis(a.z, b.z, c.z);
	return static_cast<std::uint8_t>(x + y * 2 + z * 4);
}

} // namespace

bool wt_is_valid_collision_policy(const WtCollisionPolicy &policy) noexcept {
	return std::isfinite(policy.thin_ratio_squared) &&
		policy.thin_ratio_squared >= 0.0 && policy.thin_ratio_squared < 1.0 &&
		std::isfinite(policy.activation_distance) && policy.activation_distance >= 0.0 &&
		std::isfinite(policy.deactivation_distance) &&
		policy.deactivation_distance >= policy.activation_distance &&
		policy.maximum_output_triangles > 0 &&
		policy.maximum_output_triangles <= kWtMaximumRegularChunkIndices / 3U;
}

WtCollisionRequirement wt_evaluate_collision_requirement(
	const WtCollisionPolicy &policy,
	bool currently_required,
	double distance
) noexcept {
	if (!wt_is_valid_collision_policy(policy) || !std::isfinite(distance) || distance < 0.0) {
		return WtCollisionRequirement::Invalid;
	}
	const double threshold = currently_required ?
		policy.deactivation_distance : policy.activation_distance;
	return distance <= threshold ?
		WtCollisionRequirement::Required : WtCollisionRequirement::NotRequired;
}

WtCollisionPayload::WtCollisionPayload() {
}

void WtCollisionPayload::clear() noexcept {
	key = {};
	generation = {};
	world_origin = {};
	faces.clear();
	blocks = {};
	dirty_block_mask = kWtCollisionAllBlocksMask;
	incremental_patch = false;
	regular_only = false;
	preserve_existing = false;
	metrics = {};
}

bool wt_is_valid_collision_payload(
	const WtCollisionPayload &collision
) noexcept {
	const std::size_t maximum_triangles = kWtMaximumRenderIndices / 3U;
	if (!wt_is_valid_chunk_key(collision.key) ||
		collision.generation.value == 0 ||
		collision.world_origin != wt_chunk_bounds(collision.key).minimum ||
		collision.faces.size() > kWtMaximumRenderIndices ||
		(collision.faces.size() % 3U) != 0 ||
		collision.metrics.input_triangles > maximum_triangles ||
		collision.metrics.output_triangles > maximum_triangles ||
		collision.metrics.degenerate_triangles > maximum_triangles ||
		collision.metrics.thin_triangles > maximum_triangles ||
		collision.metrics.decimated_triangles > maximum_triangles ||
		collision.metrics.output_triangles * 3U != collision.faces.size() ||
		collision.metrics.input_triangles !=
			collision.metrics.output_triangles +
			collision.metrics.degenerate_triangles +
			collision.metrics.thin_triangles +
			collision.metrics.decimated_triangles) {
		return false;
	}
	std::size_t next_face = 0;
	for (std::size_t block = 0; block < collision.blocks.size(); ++block) {
		const WtCollisionBlockRange &range = collision.blocks[block];
		if (range.first_face != next_face || (range.face_count % 3U) != 0 ||
			range.face_count > collision.faces.size() - next_face ||
			((collision.dirty_block_mask & (1U << block)) == 0 &&
				range.face_count != 0)) {
			return false;
		}
		next_face += range.face_count;
	}
	if (collision.dirty_block_mask == 0 || next_face != collision.faces.size()) {
		return false;
	}
	for (const WtVec3 &face : collision.faces) {
		if (!is_finite(face)) return false;
	}
	return true;
}

WtCollisionBuildStatus wt_build_collision_payload(
	const WtRenderPayload &render,
	const WtCollisionPolicy &policy,
	WtCollisionPayload &output
) {
	output.clear();
	if (!wt_is_valid_collision_policy(policy)) {
		return WtCollisionBuildStatus::InvalidPolicy;
	}
	if (!wt_is_valid_chunk_key(render.key) || render.generation.value == 0 ||
		render.world_origin != wt_chunk_bounds(render.key).minimum ||
		(render.indices.size() % 3U) != 0) {
		return WtCollisionBuildStatus::InvalidInput;
	}
	if (render.vertices.size() > kWtMaximumRenderVertices ||
		render.indices.size() > kWtMaximumRenderIndices) {
		return WtCollisionBuildStatus::CapacityExceeded;
	}
	output.key = render.key;
	output.generation = render.generation;
	output.world_origin = render.world_origin;
	output.metrics.input_triangles = render.indices.size() / 3;
	output.faces.reserve(render.indices.size());
	for (std::size_t triangle = 0; triangle < render.indices.size(); triangle += 3) {
		const std::uint32_t ia = render.indices[triangle];
		const std::uint32_t ib = render.indices[triangle + 1];
		const std::uint32_t ic = render.indices[triangle + 2];
		if (ia >= render.vertices.size() || ib >= render.vertices.size() ||
			ic >= render.vertices.size()) {
			output.clear();
			return WtCollisionBuildStatus::InvalidMesh;
		}
		const WtVec3 &a = render.vertices[ia].position;
		const WtVec3 &b = render.vertices[ib].position;
		const WtVec3 &c = render.vertices[ic].position;
		if (!is_finite(a) || !is_finite(b) || !is_finite(c)) {
			output.clear();
			return WtCollisionBuildStatus::InvalidMesh;
		}
		const double maximum_edge_squared = std::max({
			distance_squared(a, b), distance_squared(b, c), distance_squared(c, a)
		});
		const double area_squared = cross_squared(a, b, c);
		if (maximum_edge_squared == 0.0 || area_squared == 0.0) {
			++output.metrics.degenerate_triangles;
			continue;
		}
		if (area_squared <= maximum_edge_squared * maximum_edge_squared *
			policy.thin_ratio_squared) {
			++output.metrics.thin_triangles;
			continue;
		}
		if (output.faces.size() + 3 > kWtMaximumRenderIndices) {
			output.clear();
			return WtCollisionBuildStatus::CapacityExceeded;
		}
		output.faces.push_back(a);
		output.faces.push_back(b);
		output.faces.push_back(c);
		++output.metrics.output_triangles;
	}
	output.blocks[0] = { 0, output.faces.size() };
	for (std::size_t block = 1; block < output.blocks.size(); ++block) {
		output.blocks[block] = { output.faces.size(), 0 };
	}
	return WtCollisionBuildStatus::Ok;
}

WtCollisionBuildStatus wt_build_regular_collision_payload(
	const WtChunkMeshResult &mesh,
	WtGenerationToken generation,
	const WtCollisionPolicy &policy,
	WtCollisionPayload &output
) {
	const WtCollisionBuildStatus status = wt_build_regular_collision_patch(
		mesh, generation, policy, kWtCollisionAllBlocksMask, output
	);
	output.incremental_patch = false;
	return status;
}

WtCollisionBuildStatus wt_build_regular_collision_patch(
	const WtChunkMeshResult &mesh,
	WtGenerationToken generation,
	const WtCollisionPolicy &policy,
	std::uint8_t dirty_block_mask,
	WtCollisionPayload &output
) {
	output.clear();
	if (!wt_is_valid_collision_policy(policy)) {
		return WtCollisionBuildStatus::InvalidPolicy;
	}
	if (!wt_is_valid_chunk_key(mesh.key) || generation.value == 0 ||
		dirty_block_mask == 0 ||
		(mesh.key.lod != 0 && dirty_block_mask != kWtCollisionAllBlocksMask) ||
		mesh.world_origin != wt_chunk_bounds(mesh.key).minimum ||
		(mesh.regular.indices.size() % 3U) != 0) {
		return WtCollisionBuildStatus::InvalidInput;
	}
	if (mesh.regular.vertices.size() > kWtMaximumRegularChunkVertices ||
		mesh.regular.indices.size() > kWtMaximumRegularChunkIndices) {
		return WtCollisionBuildStatus::CapacityExceeded;
	}
	output.key = mesh.key;
	output.generation = generation;
	output.world_origin = mesh.world_origin;
	output.dirty_block_mask = dirty_block_mask;
	output.incremental_patch = true;
	output.regular_only = true;
	std::vector<CollisionTriangleCandidate> candidates;
	candidates.reserve(std::min(
		mesh.regular.indices.size() / 3U,
		policy.maximum_output_triangles
	));
	for (std::size_t triangle = 0;
			triangle < mesh.regular.indices.size(); triangle += 3U) {
		const std::uint32_t ia = mesh.regular.indices[triangle];
		const std::uint32_t ib = mesh.regular.indices[triangle + 1U];
		const std::uint32_t ic = mesh.regular.indices[triangle + 2U];
		if (ia >= mesh.regular.vertices.size() ||
			ib >= mesh.regular.vertices.size() ||
			ic >= mesh.regular.vertices.size()) {
			output.clear();
			return WtCollisionBuildStatus::InvalidMesh;
		}
		const WtVec3 &a = mesh.regular.vertices[ia].position;
		const WtVec3 &b = mesh.regular.vertices[ib].position;
		const WtVec3 &c = mesh.regular.vertices[ic].position;
		if (!is_finite(a) || !is_finite(b) || !is_finite(c)) {
			output.clear();
			return WtCollisionBuildStatus::InvalidMesh;
		}
		const std::uint8_t block = collision_block_for_triangle(
			a, b, c, mesh.key.lod
		);
		if ((dirty_block_mask & (1U << block)) == 0) continue;
		++output.metrics.input_triangles;
		const double maximum_edge_squared = std::max({
			distance_squared(a, b),
			distance_squared(b, c),
			distance_squared(c, a),
		});
		const double area_squared = cross_squared(a, b, c);
		if (maximum_edge_squared == 0.0 || area_squared == 0.0) {
			++output.metrics.degenerate_triangles;
			continue;
		}
		if (area_squared <= maximum_edge_squared * maximum_edge_squared *
			policy.thin_ratio_squared) {
			++output.metrics.thin_triangles;
			continue;
		}
		candidates.push_back({
			a,
			b,
			c,
			area_squared,
			candidates.size(),
			block,
		});
	}
	if (candidates.size() > policy.maximum_output_triangles) {
		std::nth_element(
			candidates.begin(),
			candidates.begin() + static_cast<std::ptrdiff_t>(
				policy.maximum_output_triangles
			),
			candidates.end(),
			[](const CollisionTriangleCandidate &left,
				const CollisionTriangleCandidate &right) {
				if (left.area_squared != right.area_squared) {
					return left.area_squared > right.area_squared;
				}
				return left.order < right.order;
			}
		);
		output.metrics.decimated_triangles =
			candidates.size() - policy.maximum_output_triangles;
		candidates.resize(policy.maximum_output_triangles);
		std::sort(
			candidates.begin(),
			candidates.end(),
			[](const CollisionTriangleCandidate &left,
				const CollisionTriangleCandidate &right) {
				return left.order < right.order;
			}
		);
	}
	output.faces.reserve(candidates.size() * 3U);
	for (std::size_t block = 0; block < output.blocks.size(); ++block) {
		WtCollisionBlockRange &range = output.blocks[block];
		range.first_face = output.faces.size();
		if ((dirty_block_mask & (1U << block)) != 0) {
			for (const CollisionTriangleCandidate &candidate : candidates) {
				if (candidate.block != block) continue;
				output.faces.push_back(candidate.a);
				output.faces.push_back(candidate.b);
				output.faces.push_back(candidate.c);
			}
		}
		range.face_count = output.faces.size() - range.first_face;
	}
	output.metrics.output_triangles = candidates.size();
	return WtCollisionBuildStatus::Ok;
}

WtCollisionBuildStatus wt_merge_collision_patch(
	const WtCollisionPayload &base,
	const WtCollisionPayload &patch,
	WtCollisionPayload &output
) {
	output.clear();
	if (!wt_is_valid_collision_payload(base) ||
		!wt_is_valid_collision_payload(patch) ||
		base.key != patch.key || base.world_origin != patch.world_origin ||
		base.dirty_block_mask != kWtCollisionAllBlocksMask ||
		patch.dirty_block_mask == 0 ||
		patch.generation.value <= base.generation.value) {
		return WtCollisionBuildStatus::InvalidInput;
	}
	output.key = patch.key;
	output.generation = patch.generation;
	output.world_origin = patch.world_origin;
	output.dirty_block_mask = kWtCollisionAllBlocksMask;
	output.incremental_patch = false;
	output.regular_only = true;
	output.faces.reserve(base.faces.size() + patch.faces.size());
	for (std::size_t block = 0; block < output.blocks.size(); ++block) {
		const bool dirty = (patch.dirty_block_mask & (1U << block)) != 0;
		const WtCollisionPayload &source = dirty ? patch : base;
		const WtCollisionBlockRange range = source.blocks[block];
		output.blocks[block].first_face = output.faces.size();
		output.blocks[block].face_count = range.face_count;
		output.faces.insert(
			output.faces.end(),
			source.faces.begin() + static_cast<std::ptrdiff_t>(range.first_face),
			source.faces.begin() + static_cast<std::ptrdiff_t>(
				range.first_face + range.face_count
			)
		);
	}
	output.metrics.output_triangles = output.faces.size() / 3U;
	output.metrics.input_triangles = output.metrics.output_triangles;
	return wt_is_valid_collision_payload(output) ?
		WtCollisionBuildStatus::Ok : WtCollisionBuildStatus::InvalidMesh;
}

} // namespace world_transvoxel
