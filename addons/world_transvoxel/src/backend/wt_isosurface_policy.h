#pragma once

#include <algorithm>
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

} // namespace world_transvoxel
