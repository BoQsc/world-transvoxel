#pragma once

#include "backend/wt_cell_types.h"
#include "meshing/wt_chunk_mesher.h"

namespace world_transvoxel {

WtVec3 wt_interpolated_mesh_normal(
	const WtCellSample &a,
	const WtCellSample &b,
	float isovalue
) noexcept;

void wt_finalize_deformed_triangles(WtChunkMeshBuffer &buffer);

}
