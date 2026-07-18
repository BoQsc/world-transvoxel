#include "storage/wt_procedural_cave_field.h"

#include <algorithm>
#include <cmath>

namespace world_transvoxel {
namespace {

constexpr double kCavePrimitiveBlendRadius = 4.0;
constexpr double kCaveTerrainBlendRadius = 3.0;

double smooth_max(double a, double b, double radius) noexcept {
	if (!(radius > 0.0)) return std::max(a, b);
	const double blend = std::clamp(
		0.5 + 0.5 * (a - b) / radius,
		0.0,
		1.0
	);
	return b + (a - b) * blend +
		radius * blend * (1.0 - blend);
}

double smooth_min(double a, double b, double radius) noexcept {
	return -smooth_max(-a, -b, radius);
}

double ellipsoid_distance(
	double x,
	double y,
	double z,
	double center_x,
	double center_y,
	double center_z,
	double radius_x,
	double radius_y,
	double radius_z
) noexcept {
	const double px = x - center_x;
	const double py = y - center_y;
	const double pz = z - center_z;
	const double qx = px / radius_x;
	const double qy = py / radius_y;
	const double qz = pz / radius_z;
	const double normalized_length = std::sqrt(
		qx * qx + qy * qy + qz * qz
	);
	const double gradient_length = std::sqrt(
		(px / (radius_x * radius_x)) * (px / (radius_x * radius_x)) +
		(py / (radius_y * radius_y)) * (py / (radius_y * radius_y)) +
		(pz / (radius_z * radius_z)) * (pz / (radius_z * radius_z))
	);
	if (gradient_length <= 1.0e-12) {
		return -std::min({ radius_x, radius_y, radius_z });
	}
	return normalized_length * (normalized_length - 1.0) /
		gradient_length;
}

double capsule_distance(
	double x,
	double y,
	double z,
	double ax,
	double ay,
	double az,
	double bx,
	double by,
	double bz,
	double radius
) noexcept {
	const double abx = bx - ax;
	const double aby = by - ay;
	const double abz = bz - az;
	const double apx = x - ax;
	const double apy = y - ay;
	const double apz = z - az;
	const double length_squared = std::max(
		abx * abx + aby * aby + abz * abz,
		1.0
	);
	const double t = std::clamp(
		(apx * abx + apy * aby + apz * abz) / length_squared,
		0.0,
		1.0
	);
	const double dx = x - (ax + abx * t);
	const double dy = y - (ay + aby * t);
	const double dz = z - (az + abz * t);
	return std::sqrt(dx * dx + dy * dy + dz * dz) - radius;
}

} // namespace

WtProceduralCaveField wt_sample_reference_cave_field(
	double x,
	double y,
	double z
) noexcept {
	const double main_chamber = ellipsoid_distance(
		x, y, z,
		1024.0, -20.0, 1024.0,
		170.0, 38.0, 130.0
	);
	const double entrance_chamber = ellipsoid_distance(
		x, y, z,
		900.0, -4.0, 1030.0,
		68.0, 32.0, 58.0
	);
	const double surface_portal = capsule_distance(
		x, y, z,
		900.0, 52.0, 970.0,
		900.0, -4.0, 1030.0,
		26.0
	);
	const double entry_tunnel = capsule_distance(
		x, y, z,
		900.0, -4.0, 1030.0,
		1032.0, -18.0, 1024.0,
		24.0
	);
	const double side_gallery = capsule_distance(
		x, y, z,
		1050.0, -18.0, 1024.0,
		1190.0, -10.0, 1110.0,
		18.0
	);
	double cave_distance = smooth_min(
		main_chamber,
		entrance_chamber,
		kCavePrimitiveBlendRadius
	);
	cave_distance = smooth_min(
		cave_distance,
		surface_portal,
		kCavePrimitiveBlendRadius
	);
	cave_distance = smooth_min(
		cave_distance,
		entry_tunnel,
		kCavePrimitiveBlendRadius
	);
	cave_distance = smooth_min(
		cave_distance,
		side_gallery,
		kCavePrimitiveBlendRadius
	);
	return { cave_distance, -cave_distance };
}

double wt_apply_reference_cave_density(
	double terrain_density,
	double x,
	double y,
	double z
) noexcept {
	const WtProceduralCaveField cave = wt_sample_reference_cave_field(x, y, z);
	return smooth_max(
		terrain_density,
		cave.air_density,
		kCaveTerrainBlendRadius
	);
}

WtProceduralCaveField wt_sample_four_biome_cave_field(
	double x,
	double y,
	double z,
	const std::array<double, kWtFourBiomeCaveCount> &portal_surface_y
) noexcept {
	if (x >= 330.0 && x <= 495.0 && z >= 495.0 && z <= 615.0 &&
		y >= portal_surface_y[0] - 60.0 && y <= portal_surface_y[0] + 20.0) {
		const double portal = capsule_distance(
			x, y, z,
			360.0, portal_surface_y[0] + 2.0, 520.0,
			430.0, portal_surface_y[0] - 22.0, 560.0,
			12.0
		);
		const double chamber = ellipsoid_distance(
			x, y, z,
			445.0, portal_surface_y[0] - 25.0, 570.0,
			38.0, 20.0, 32.0
		);
		const double distance = smooth_min(
			portal, chamber, kCavePrimitiveBlendRadius
		);
		return { distance, -distance };
	}
	if (x >= 1540.0 && x <= 1665.0 && z >= 495.0 && z <= 670.0 &&
		y >= portal_surface_y[1] - 65.0 && y <= portal_surface_y[1] + 20.0) {
		const double portal = capsule_distance(
			x, y, z,
			1570.0, portal_surface_y[1] + 2.0, 520.0,
			1600.0, portal_surface_y[1] - 25.0, 600.0,
			13.0
		);
		const double chamber = ellipsoid_distance(
			x, y, z,
			1605.0, portal_surface_y[1] - 28.0, 615.0,
			42.0, 21.0, 36.0
		);
		const double distance = smooth_min(
			portal, chamber, kCavePrimitiveBlendRadius
		);
		return { distance, -distance };
	}
	if (x >= 400.0 && x <= 590.0 && z >= 1435.0 && z <= 1570.0 &&
		y >= portal_surface_y[2] - 65.0 && y <= portal_surface_y[2] + 20.0) {
		const double portal = capsule_distance(
			x, y, z,
			430.0, portal_surface_y[2] + 2.0, 1540.0,
			515.0, portal_surface_y[2] - 27.0, 1500.0,
			12.0
		);
		const double chamber = ellipsoid_distance(
			x, y, z,
			530.0, portal_surface_y[2] - 30.0, 1490.0,
			40.0, 20.0, 34.0
		);
		const double distance = smooth_min(
			portal, chamber, kCavePrimitiveBlendRadius
		);
		return { distance, -distance };
	}
	return { 1000000.0, -1000000.0 };
}

double wt_apply_four_biome_cave_density(
	double terrain_density,
	double x,
	double y,
	double z,
	const std::array<double, kWtFourBiomeCaveCount> &portal_surface_y
) noexcept {
	const WtProceduralCaveField cave = wt_sample_four_biome_cave_field(
		x, y, z, portal_surface_y
	);
	return smooth_max(
		terrain_density,
		cave.air_density,
		kCaveTerrainBlendRadius
	);
}

} // namespace world_transvoxel
