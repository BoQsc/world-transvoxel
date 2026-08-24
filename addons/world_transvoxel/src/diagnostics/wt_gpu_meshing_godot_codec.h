#pragma once

#include "diagnostics/wt_gpu_meshing_differential_backend.h"

#include <godot_cpp/variant/dictionary.hpp>

#include <vector>

namespace world_transvoxel {

godot::Dictionary wt_gpu_meshing_shadow_cell_batch(
	const std::vector<WtRecordedMeshingCell> &records
);

} // namespace world_transvoxel
