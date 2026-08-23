#pragma once

#include "backend/wt_meshing_backend.h"

#include <array>
#include <cstdint>

namespace world_transvoxel {

class WtTransvoxelMitBackend final : public WtMeshingBackend {
public:
	const WtMeshingBackendInfo &get_info() const noexcept override;
	bool is_available() const noexcept override;
	WtCellStatus mesh_regular_cell(
		const WtRegularCellInput &input,
		WtCellMesh &output,
		WtCellMeshingScratch &scratch
	) const noexcept override;
	WtCellStatus mesh_transition_cell(
		const WtTransitionCellInput &input,
		WtCellMesh &output,
		WtCellMeshingScratch &scratch
	) const noexcept override;
};

struct WtTransvoxelTablePack {
	std::array<std::uint32_t, 256> regular_cell_class{};
	std::array<std::uint32_t, 16 * 16> regular_cell_data{};
	std::array<std::uint32_t, 256 * 12> regular_vertex_data{};
	std::array<std::uint32_t, 512> transition_cell_class{};
	std::array<std::uint32_t, 56 * 37> transition_cell_data{};
	std::array<std::uint32_t, 512 * 12> transition_vertex_data{};
};

const WtMeshingBackend &wt_get_transvoxel_mit_backend() noexcept;
const WtTransvoxelTablePack &wt_get_transvoxel_mit_table_pack() noexcept;

} // namespace world_transvoxel
