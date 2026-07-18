#pragma once

#include <array>
#include <cstddef>

namespace world_transvoxel {

constexpr std::size_t kWtReferenceRoadSegmentCount = 6;

struct WtReferenceRoadSegment {
	double ax = 0.0;
	double az = 0.0;
	double bx = 0.0;
	double bz = 0.0;
};

struct WtProceduralRoadField {
	double surface = 0.0;
	double distance = 0.0;
};

const std::array<
	WtReferenceRoadSegment,
	kWtReferenceRoadSegmentCount
> &wt_reference_road_segments() noexcept;

WtProceduralRoadField wt_sample_reference_road_field(
	double base_surface,
	double x,
	double z,
	const std::array<double, kWtReferenceRoadSegmentCount> &height_a,
	const std::array<double, kWtReferenceRoadSegmentCount> &height_b
) noexcept;

bool wt_reference_road_has_asphalt(
	double distance,
	double depth
) noexcept;

} // namespace world_transvoxel
