#pragma once

#include "render/wt_base_coverage_atlas.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

struct WtBaseCoverageGpuRoot {
	WtChunkKey key;
	std::int32_t draw_index = -1; // -1 is a proven-empty root.
};

struct WtBaseCoverageGpuPack {
	std::vector<std::uint8_t> positions; // world-space R32G32B32_SFLOAT
	std::vector<std::uint8_t> normals; // octahedral R16G16_SNORM
	std::vector<std::uint8_t> metadata; // material, authored flag: R16G16_UINT
	std::vector<std::uint8_t> indices; // local-to-root UINT32
	std::vector<std::uint8_t> indirect; // indexed VkDrawIndexedIndirectCommand
	std::vector<WtBaseCoverageGpuRoot> roots;
	std::size_t vertex_count = 0;
	std::size_t index_count = 0;
};

// Converts a fully validated atlas to one bounded, indexed GPU allocation.
// The source format uses chunk-local positions; the existing raster shader
// consumes world positions. Draws remain individually switchable per root.
bool wt_pack_base_coverage_for_gpu(
	const WtBaseCoverageAtlasView &atlas,
	WtBaseCoverageGpuPack &output
);

} // namespace world_transvoxel
