#include "storage/wt_procedural_road_field.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace world_transvoxel {
namespace {

constexpr std::array<
	WtReferenceRoadSegment,
	kWtReferenceRoadSegmentCount
> kReferenceRoadSegments = {{
	{ 720.0, 820.0, 900.0, 820.0 },
	{ 900.0, 820.0, 1080.0, 820.0 },
	{ 900.0, 680.0, 900.0, 820.0 },
	{ 900.0, 820.0, 900.0, 960.0 },
	{ 720.0, 820.0, 760.0, 900.0 },
	{ 900.0, 960.0, 1040.0, 940.0 },
}};

constexpr std::array<
	WtReferenceRoadSegment,
	kWtExpansiveRoadSegmentCount
> kExpansiveRoadSegments = {{
	{ 360.0, 360.0, 820.0, 420.0 },
	{ 820.0, 420.0, 1020.0, 360.0 },
	{ 1020.0, 360.0, 1640.0, 380.0 },
	{ 1640.0, 380.0, 1700.0, 900.0 },
	{ 1700.0, 900.0, 1600.0, 1600.0 },
	{ 1600.0, 1600.0, 1020.0, 1700.0 },
	{ 1020.0, 1700.0, 400.0, 1600.0 },
	{ 400.0, 1600.0, 300.0, 1100.0 },
	{ 300.0, 1100.0, 400.0, 850.0 },
	{ 400.0, 850.0, 360.0, 360.0 },
	{ 400.0, 850.0, 760.0, 1040.0 },
	{ 760.0, 1040.0, 1024.0, 1024.0 },
	{ 1024.0, 1024.0, 1280.0, 1000.0 },
	{ 1280.0, 1000.0, 1700.0, 900.0 },
	{ 1024.0, 1024.0, 1020.0, 1700.0 },
	{ 1024.0, 1024.0, 1020.0, 360.0 },
	{ 760.0, 1040.0, 1020.0, 1700.0 },
	{ 1280.0, 1000.0, 1600.0, 1600.0 },
}};

constexpr double kReferenceRoadHalfWidth = 6.0;
constexpr double kReferenceRoadShoulderWidth = 5.0;
constexpr double kReferenceRoadAsphaltDepth = 3.0;
constexpr double kExpansiveRoadHalfWidth = 8.0;
constexpr double kExpansiveRoadShoulderWidth = 8.0;
constexpr double kExpansiveRoadAsphaltDepth = 3.5;

double smoothstep(double edge_a, double edge_b, double value) noexcept {
	if (!(edge_b > edge_a)) {
		return value >= edge_b ? 1.0 : 0.0;
	}
	const double t = std::clamp(
		(value - edge_a) / (edge_b - edge_a),
		0.0,
		1.0
	);
	return t * t * (3.0 - 2.0 * t);
}

template <std::size_t SegmentCount>
WtProceduralRoadField sample_road_field(
	double base_surface,
	double x,
	double z,
	const std::array<WtReferenceRoadSegment, SegmentCount> &segments,
	const std::array<double, SegmentCount> &height_a,
	const std::array<double, SegmentCount> &height_b,
	double half_width,
	double shoulder_width
) noexcept {
	const double shoulder_limit = half_width + shoulder_width;
	double minimum_distance = std::numeric_limits<double>::infinity();
	double grade_sum = 0.0;
	double grade_weight_sum = 0.0;
	double surface_blend = 0.0;
	for (std::size_t index = 0; index < segments.size(); ++index) {
		const WtReferenceRoadSegment &segment = segments[index];
		if (x < std::min(segment.ax, segment.bx) - shoulder_limit ||
			x > std::max(segment.ax, segment.bx) + shoulder_limit ||
			z < std::min(segment.az, segment.bz) - shoulder_limit ||
			z > std::max(segment.az, segment.bz) + shoulder_limit) {
			continue;
		}
		const double abx = segment.bx - segment.ax;
		const double abz = segment.bz - segment.az;
		const double apx = x - segment.ax;
		const double apz = z - segment.az;
		const double length_squared = std::max(abx * abx + abz * abz, 1.0);
		const double t = std::clamp(
			(apx * abx + apz * abz) / length_squared,
			0.0,
			1.0
		);
		const double dx = x - (segment.ax + abx * t);
		const double dz = z - (segment.az + abz * t);
		const double distance = std::sqrt(dx * dx + dz * dz);
		minimum_distance = std::min(minimum_distance, distance);
		if (distance >= shoulder_limit) continue;
		const double weight = 1.0 - smoothstep(
			half_width,
			shoulder_limit,
			distance
		);
		const double grade = height_a[index] +
			(height_b[index] - height_a[index]) * t;
		grade_sum += grade * weight;
		grade_weight_sum += weight;
		surface_blend = std::max(surface_blend, weight);
	}
	if (grade_weight_sum <= 0.0) {
		return { base_surface, minimum_distance };
	}
	const double road_grade = grade_sum / grade_weight_sum;
	return {
		base_surface + (road_grade - base_surface) * surface_blend,
		minimum_distance,
	};
}

} // namespace

const std::array<
	WtReferenceRoadSegment,
	kWtReferenceRoadSegmentCount
> &wt_reference_road_segments() noexcept {
	return kReferenceRoadSegments;
}

const std::array<
	WtReferenceRoadSegment,
	kWtExpansiveRoadSegmentCount
> &wt_expansive_road_segments() noexcept {
	return kExpansiveRoadSegments;
}

WtProceduralRoadField wt_sample_reference_road_field(
	double base_surface,
	double x,
	double z,
	const std::array<double, kWtReferenceRoadSegmentCount> &height_a,
	const std::array<double, kWtReferenceRoadSegmentCount> &height_b
) noexcept {
	return sample_road_field(
		base_surface, x, z, kReferenceRoadSegments,
		height_a, height_b,
		kReferenceRoadHalfWidth, kReferenceRoadShoulderWidth
	);
}

bool wt_reference_road_has_asphalt(
	double distance,
	double depth
) noexcept {
	return distance <= kReferenceRoadHalfWidth &&
		depth >= 0.0 && depth <= kReferenceRoadAsphaltDepth;
}

WtProceduralRoadField wt_sample_expansive_road_field(
	double base_surface,
	double x,
	double z,
	const std::array<double, kWtExpansiveRoadSegmentCount> &height_a,
	const std::array<double, kWtExpansiveRoadSegmentCount> &height_b
) noexcept {
	return sample_road_field(
		base_surface, x, z, kExpansiveRoadSegments,
		height_a, height_b,
		kExpansiveRoadHalfWidth, kExpansiveRoadShoulderWidth
	);
}

bool wt_expansive_road_has_asphalt(
	double distance,
	double depth
) noexcept {
	return distance <= kExpansiveRoadHalfWidth &&
		depth >= 0.0 && depth <= kExpansiveRoadAsphaltDepth;
}

} // namespace world_transvoxel
