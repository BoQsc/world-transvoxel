#include "backend/wt_cell_types.h"

namespace world_transvoxel {

WtCellMeshingScratch &wt_get_thread_cell_meshing_scratch() noexcept {
	thread_local WtCellMeshingScratch scratch;
	return scratch;
}

} // namespace world_transvoxel
