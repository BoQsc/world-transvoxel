#pragma once

namespace world_transvoxel {

// M1 production defaults. M2 may make the chunk size configurable, but these
// dimensions remain the tested baseline and serialization-independent default.
constexpr unsigned int kWtDefaultChunkCellsPerAxis = 16;
constexpr unsigned int kWtChunkNegativeSamplePadding = 1;
constexpr unsigned int kWtChunkPositiveSamplePadding = 2;
constexpr unsigned int kWtChunkMeshingSamplesPerAxis =
	kWtDefaultChunkCellsPerAxis +
	kWtChunkNegativeSamplePadding +
	kWtChunkPositiveSamplePadding;

static_assert(kWtChunkMeshingSamplesPerAxis == 19);

} // namespace world_transvoxel
