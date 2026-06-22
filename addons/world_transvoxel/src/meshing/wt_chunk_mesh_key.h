#pragma once

#include "meshing/wt_chunk_mesher.h"

namespace world_transvoxel {

WtChunkVertexKey wt_make_chunk_vertex_key(const WtCellVertex &vertex) noexcept;

}
