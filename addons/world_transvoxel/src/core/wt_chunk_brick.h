#pragma once

#include "core/wt_chunk_key.h"

#include <array>
#include <cstdint>

namespace world_transvoxel {

constexpr std::int64_t kWtRegularBrickCellsPerAxis = 8;
constexpr std::uint8_t kWtRegularBricksPerAxis = 2;
constexpr std::uint8_t kWtRegularBrickCount = 8;
constexpr std::uint8_t kWtAllRegularBricksMask = 0xff;

struct WtRegularBrickKey {
	WtChunkKey chunk;
	std::uint8_t index = 0;

	bool operator==(const WtRegularBrickKey &other) const noexcept;
	bool operator!=(const WtRegularBrickKey &other) const noexcept;
	bool operator<(const WtRegularBrickKey &other) const noexcept;
};

struct WtRegularBrickSet {
	std::array<WtRegularBrickKey, kWtRegularBrickCount> keys;
	std::uint8_t count = 0;
	std::uint8_t mask = 0;
};

bool wt_is_valid_regular_brick_key(const WtRegularBrickKey &key) noexcept;
WtGridPoint wt_regular_brick_coordinate(std::uint8_t index) noexcept;
WtChunkBounds wt_regular_brick_bounds(const WtRegularBrickKey &key) noexcept;
WtRegularBrickSet wt_regular_bricks_from_mask(
	const WtChunkKey &chunk,
	std::uint8_t mask
) noexcept;

// Dirty sample bounds are inclusive. A sample on a brick boundary belongs to
// both adjacent cell dependencies, which supplies the one-sample extraction
// halo without broadening unrelated bricks.
std::uint8_t wt_regular_brick_mask_for_sample_bounds(
	const WtChunkKey &chunk,
	const WtGridPoint &dirty_minimum,
	const WtGridPoint &dirty_maximum
) noexcept;

// A direct child chunk covers exactly one regular meshlet brick in its parent.
// This relationship is the unit used by localized 2:1 LOD publication.
bool wt_direct_child_parent_brick(
	const WtChunkKey &child,
	const WtChunkKey &parent,
	WtRegularBrickKey &brick
) noexcept;

} // namespace world_transvoxel
