#pragma once

#include "diagnostics/wt_gpu_meshing_shadow.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace world_transvoxel {

struct WtGpuMeshingInputPack {
	std::vector<float> field_values;
	std::vector<std::int32_t> field_meta;
	std::vector<std::int32_t> cell_headers;
	std::vector<float> cell_origins;
	std::vector<float> cell_options;
	std::vector<std::int32_t> sample_references;
	std::array<std::int32_t, 16> config{};
	WtVec3 bounds_min;
	WtVec3 bounds_max;
	std::size_t cell_count = 0;
	std::size_t sample_count = 0;
	std::size_t packed_byte_count = 0;
};

bool wt_pack_gpu_meshing_input(
	const WtGpuMeshingShadowRequest &request,
	WtGpuMeshingInputPack &output,
	std::string &error
);

} // namespace world_transvoxel
