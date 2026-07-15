#pragma once

#include "backend/wt_cell_types.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>

namespace world_transvoxel {

inline double wt_regularized_isosurface_alpha(double alpha) noexcept {
	constexpr double kMinimumIsosurfaceEndpointFraction = 1.0 / 32.0;
	if (!std::isfinite(alpha)) {
		return 0.5;
	}
	alpha = std::max(0.0, std::min(1.0, alpha));
	return std::max(
		kMinimumIsosurfaceEndpointFraction,
		std::min(1.0 - kMinimumIsosurfaceEndpointFraction, alpha)
	);
}

WtVec3 wt_canonical_chunk_position(
	const WtCellVertex &vertex,
	const std::array<WtVec3, kWtTransitionTopologySampleCount> &endpoint_positions,
	const WtCellSample *samples,
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
