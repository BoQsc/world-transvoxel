#pragma once

#include "core/wt_chunk_key.h"

#include <cstdint>

namespace world_transvoxel {

enum class WtProceduralWorldMode : std::uint8_t {
	Terrain = 0,
	Flat = 1,
	RollingHillsCave = 2,
	RollingHillsCaveRoads = 3,
	FourBiomesLakesCavesRoads = 4,
};

enum class WtProceduralBottomBoundaryPolicy : std::uint8_t {
	Open = 0,
	Sealed = 1,
	Bedrock = 2,
};

constexpr std::uint16_t kWtProceduralBedrockMaterial = 7;

struct WtProceduralWorldDescriptor {
	std::uint32_t chunk_count_x = 0;
	std::uint32_t chunk_count_y = 8;
	std::uint32_t chunk_count_z = 0;
	std::int32_t chunk_y = 0;
	std::uint64_t source_revision = 0;
	std::uint64_t world_revision = 0;
	std::uint32_t seed = 1;
	WtProceduralWorldMode mode = WtProceduralWorldMode::Terrain;
	WtProceduralBottomBoundaryPolicy bottom_boundary_policy =
		WtProceduralBottomBoundaryPolicy::Open;
	std::uint16_t bottom_boundary_thickness_cells = 0;
};

inline bool wt_procedural_bottom_boundary_protects_sample(
	const WtProceduralWorldDescriptor &descriptor,
	const WtGridPoint &point
) noexcept {
	if (descriptor.bottom_boundary_policy ==
			WtProceduralBottomBoundaryPolicy::Open) {
		return false;
	}
	const std::int64_t bottom_boundary_top =
		static_cast<std::int64_t>(descriptor.chunk_y) *
			kWtChunkCellsPerAxis +
		static_cast<std::int64_t>(descriptor.bottom_boundary_thickness_cells);
	return point.y <= bottom_boundary_top;
}

bool wt_same_procedural_world_geometry(
	const WtProceduralWorldDescriptor &left,
	const WtProceduralWorldDescriptor &right
) noexcept;

} // namespace world_transvoxel
