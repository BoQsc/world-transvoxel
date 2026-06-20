#pragma once

namespace world_transvoxel {

// M1 production defaults retained by the locked M2 chunk contract.
constexpr unsigned int kWtDefaultChunkCellsPerAxis = 16;
constexpr unsigned int kWtChunkNegativeSamplePadding = 1;
constexpr unsigned int kWtChunkPositiveSamplePadding = 2;
constexpr unsigned int kWtChunkMeshingSamplesPerAxis =
	kWtDefaultChunkCellsPerAxis +
	kWtChunkNegativeSamplePadding +
	kWtChunkPositiveSamplePadding;

static_assert(kWtChunkMeshingSamplesPerAxis == 19);

} // namespace world_transvoxel
