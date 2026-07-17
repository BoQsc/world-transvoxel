#pragma once

#include "meshing/wt_chunk_mesher.h"

namespace world_transvoxel {

bool wt_preserve_transition_face_constraints(
	WtChunkMeshBuffer &buffer,
	WtChunkFace face,
	float extent
);

} // namespace world_transvoxel
