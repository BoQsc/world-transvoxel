#pragma once

#include "render/wt_render_payload.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

constexpr double kWtDefaultCollisionThinRatioSquared = 1.0e-12;
constexpr double kWtDefaultCollisionActivationDistance = 96.0;
constexpr double kWtDefaultCollisionDeactivationDistance = 128.0;

struct WtCollisionPolicy {
	double thin_ratio_squared = kWtDefaultCollisionThinRatioSquared;
	double activation_distance = kWtDefaultCollisionActivationDistance;
	double deactivation_distance = kWtDefaultCollisionDeactivationDistance;
};

enum class WtCollisionRequirement : std::uint8_t {
	Required,
	NotRequired,
	Invalid,
};

bool wt_is_valid_collision_policy(const WtCollisionPolicy &policy) noexcept;
WtCollisionRequirement wt_evaluate_collision_requirement(
	const WtCollisionPolicy &policy,
	bool currently_required,
	double distance
) noexcept;

struct WtCollisionBuildMetrics {
	std::size_t input_triangles = 0;
	std::size_t output_triangles = 0;
	std::size_t degenerate_triangles = 0;
	std::size_t thin_triangles = 0;
};

struct WtCollisionPayload {
	WtChunkKey key;
	WtGenerationToken generation;
	WtGridPoint world_origin;
	std::vector<WtVec3> faces;
	WtCollisionBuildMetrics metrics;

	WtCollisionPayload();
	void clear() noexcept;
};

enum class WtCollisionBuildStatus : std::uint8_t {
	Ok,
	InvalidPolicy,
	InvalidInput,
	InvalidMesh,
	CapacityExceeded,
};

WtCollisionBuildStatus wt_build_collision_payload(
	const WtRenderPayload &render,
	const WtCollisionPolicy &policy,
	WtCollisionPayload &output
);

} // namespace world_transvoxel
