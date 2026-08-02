#pragma once

#include "backend/wt_cell_types.h"
#include "backend/wt_isosurface_policy.h"

#include <array>
#include <cstdint>

namespace world_transvoxel {

WtVec3 wt_canonical_chunk_position(
	const WtCellVertex &vertex,
	const std::array<WtVec3, kWtTransitionTopologySampleCount> &endpoint_positions,
	const WtCellSample *samples,
	float isovalue
) noexcept;

WtVec3 wt_canonical_edge_position(
	const WtVec3 &endpoint_a,
	const WtVec3 &endpoint_b,
	const WtCellSample &sample_a,
	const WtCellSample &sample_b,
	float isovalue
) noexcept;

const WtCellSample &wt_solid_isosurface_endpoint_sample(
	const WtCellSample &sample_a,
	const WtCellSample &sample_b,
	float isovalue
) noexcept;

WtVec3 wt_deform_chunk_position(
	WtVec3 position,
	const WtVec3 &normal,
	std::uint8_t transition_mask,
	float cell_size,
	float width,
	float extent,
	int primary_transition_face
) noexcept;

WtVec3 wt_snap_chunk_position(WtVec3 position) noexcept;

} // namespace world_transvoxel
