#pragma once

#include "render/wt_render_payload.h"
#include "meshing/wt_chunk_mesher.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

namespace world_transvoxel {

constexpr double kWtDefaultCollisionThinRatioSquared = 1.0e-12;
constexpr double kWtDefaultCollisionActivationDistance = 96.0;
constexpr double kWtDefaultCollisionDeactivationDistance = 128.0;
// Runtime collision must preserve every valid regular-cell triangle. A
// largest-area cap can remove caves, crater floors, and thin constructed
// surfaces while the authoritative field and render mesh still contain them.
// Collision residency is bounded by demand; per-chunk correctness is not a
// permissible residency tradeoff.
constexpr std::size_t kWtDefaultCollisionMaximumOutputTriangles =
	kWtMaximumRegularChunkIndices / 3U;

struct WtCollisionPolicy {
	double thin_ratio_squared = kWtDefaultCollisionThinRatioSquared;
	double activation_distance = kWtDefaultCollisionActivationDistance;
	double deactivation_distance = kWtDefaultCollisionDeactivationDistance;
	std::size_t maximum_output_triangles =
		kWtDefaultCollisionMaximumOutputTriangles;
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
	std::size_t decimated_triangles = 0;
};

constexpr std::size_t kWtCollisionBlockCount = 8;
constexpr std::uint8_t kWtCollisionAllBlocksMask = 0xff;

struct WtCollisionBlockRange {
	std::size_t first_face = 0;
	std::size_t face_count = 0;
	bool operator==(const WtCollisionBlockRange &other) const noexcept {
		return first_face == other.first_face && face_count == other.face_count;
	}
};

struct WtCollisionPayload {
	WtChunkKey key;
	WtGenerationToken generation;
	WtGridPoint world_origin;
	std::vector<WtVec3> faces;
	std::array<WtCollisionBlockRange, kWtCollisionBlockCount> blocks;
	std::uint8_t dirty_block_mask = kWtCollisionAllBlocksMask;
	bool incremental_patch = false;
	bool regular_only = false;
	bool preserve_existing = false;
	WtCollisionBuildMetrics metrics;

	WtCollisionPayload();
	void clear() noexcept;
};

bool wt_is_valid_collision_payload(
	const WtCollisionPayload &collision
) noexcept;

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

// Builds physics from the regular cell mesh only. Transvoxel transition
// triangles are render-side seam geometry and are intentionally excluded from
// the authoritative collision surface.
WtCollisionBuildStatus wt_build_regular_collision_payload(
	const WtChunkMeshResult &mesh,
	WtGenerationToken generation,
	const WtCollisionPolicy &policy,
	WtCollisionPayload &output
);

// Produces a complete generation token with geometry only for the selected
// LOD0 8-cubed collision blocks. A sink retains every unselected block from
// the preceding generation and publishes the selected set atomically.
WtCollisionBuildStatus wt_build_regular_collision_patch(
	const WtChunkMeshResult &mesh,
	WtGenerationToken generation,
	const WtCollisionPolicy &policy,
	std::uint8_t dirty_block_mask,
	WtCollisionPayload &output
);

WtCollisionBuildStatus wt_merge_collision_patch(
	const WtCollisionPayload &base,
	const WtCollisionPayload &patch,
	WtCollisionPayload &output
);

} // namespace world_transvoxel
