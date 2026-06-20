#include "physics/wt_collision_builder.h"

#include <algorithm>
#include <cmath>

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

} // namespace

bool wt_is_valid_collision_policy(const WtCollisionPolicy &policy) noexcept {
	return std::isfinite(policy.thin_ratio_squared) &&
		policy.thin_ratio_squared >= 0.0 && policy.thin_ratio_squared < 1.0 &&
		std::isfinite(policy.activation_distance) && policy.activation_distance >= 0.0 &&
		std::isfinite(policy.deactivation_distance) &&
		policy.deactivation_distance >= policy.activation_distance;
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
	metrics = {};
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
	return WtCollisionBuildStatus::Ok;
}

} // namespace world_transvoxel
