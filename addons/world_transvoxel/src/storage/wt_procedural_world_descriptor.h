#pragma once

#include <cstdint>

namespace world_transvoxel {

enum class WtProceduralWorldMode : std::uint8_t {
	Terrain = 0,
	Flat = 1,
	RollingHillsCave = 2,
	RollingHillsCaveRoads = 3,
	FourBiomesLakesCavesRoads = 4,
};

struct WtProceduralWorldDescriptor {
	std::uint32_t chunk_count_x = 0;
	std::uint32_t chunk_count_y = 8;
	std::uint32_t chunk_count_z = 0;
	std::int32_t chunk_y = 0;
	std::uint64_t source_revision = 0;
	std::uint64_t world_revision = 0;
	std::uint32_t seed = 1;
	WtProceduralWorldMode mode = WtProceduralWorldMode::Terrain;
};

bool wt_same_procedural_world_geometry(
	const WtProceduralWorldDescriptor &left,
	const WtProceduralWorldDescriptor &right
) noexcept;

} // namespace world_transvoxel
