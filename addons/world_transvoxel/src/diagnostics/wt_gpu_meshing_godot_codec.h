#pragma once

#include "diagnostics/wt_gpu_meshing_differential_backend.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <vector>

namespace world_transvoxel {

godot::Dictionary wt_gpu_meshing_shadow_cell_batch(
	const std::vector<WtRecordedMeshingCell> &records
);

bool wt_parse_gpu_replay_cells(
	const godot::Array &values,
	std::vector<WtReplayMeshingCell> &cells,
	godot::String &error
);

} // namespace world_transvoxel
