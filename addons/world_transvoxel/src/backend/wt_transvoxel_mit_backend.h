#pragma once

#include "backend/wt_meshing_backend.h"

namespace world_transvoxel {

class WtTransvoxelMitBackend final : public WtMeshingBackend {
public:
	const WtMeshingBackendInfo &get_info() const noexcept override;
	bool is_available() const noexcept override;
};

const WtMeshingBackend &wt_get_transvoxel_mit_backend() noexcept;

} // namespace world_transvoxel
