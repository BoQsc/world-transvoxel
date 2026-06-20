#pragma once

#include "backend/wt_cell_types.h"

namespace world_transvoxel {

struct WtMeshingBackendInfo {
	const char *id;
	const char *license;
	const char *upstream_revision;
	unsigned int regular_case_count;
	unsigned int transition_case_count;
	bool official_reference;
};

class WtMeshingBackend {
public:
	virtual ~WtMeshingBackend() = default;

	virtual const WtMeshingBackendInfo &get_info() const noexcept = 0;
	virtual bool is_available() const noexcept = 0;
	virtual WtCellStatus mesh_regular_cell(
		const WtRegularCellInput &input,
		WtCellMesh &output,
		WtCellMeshingScratch &scratch
	) const noexcept = 0;
	virtual WtCellStatus mesh_transition_cell(
		const WtTransitionCellInput &input,
		WtCellMesh &output,
		WtCellMeshingScratch &scratch
	) const noexcept = 0;
};

} // namespace world_transvoxel
