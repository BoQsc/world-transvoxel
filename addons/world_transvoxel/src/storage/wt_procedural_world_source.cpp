#include "storage/wt_procedural_world_source.h"

#include "bake/wt_chunk_baker.h"
#include "storage/wt_procedural_cave_field.h"
#include "storage/wt_procedural_road_field.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace world_transvoxel {
namespace {

std::uint32_t ceil_divide_u32(
	std::uint32_t value,
	std::uint32_t divisor
) noexcept {
	return (value + divisor - 1U) / divisor;
}

std::int32_t floor_divide_i32(
	std::int32_t value,
	std::int32_t divisor
) noexcept {
	return value >= 0 ? value / divisor :
		-static_cast<std::int32_t>(
			(-static_cast<std::int64_t>(value) + divisor - 1) / divisor
		);
}

std::uint32_t lod_span(std::uint8_t lod) noexcept {
	return std::uint32_t{ 1 } << lod;
}

std::uint32_t vertical_layer_count(
	const WtProceduralWorldDescriptor &descriptor,
	std::uint8_t lod
) noexcept {
	return ceil_divide_u32(descriptor.chunk_count_y, lod_span(lod));
}

std::int32_t vertical_origin(
	const WtProceduralWorldDescriptor &descriptor,
	std::uint8_t lod
) noexcept {
	return floor_divide_i32(descriptor.chunk_y, static_cast<std::int32_t>(lod_span(lod)));
}

float regularized_density(double density) noexcept {
	constexpr double kDensityEpsilon = 0.01;
	if (!std::isfinite(density)) {
		return std::numeric_limits<float>::quiet_NaN();
	}
	if (density > -kDensityEpsilon && density < kDensityEpsilon) {
		return density < 0.0 ? -static_cast<float>(kDensityEpsilon) :
			static_cast<float>(kDensityEpsilon);
	}
	return static_cast<float>(density);
}

double procedural_wave(
	std::int64_t x,
	std::int64_t y,
	std::int64_t z,
	double seed_phase
) noexcept {
	return
		std::sin(static_cast<double>(x) * 0.031 + seed_phase * 1.73) *
			0.47 +
		std::cos(static_cast<double>(z) * 0.027 - seed_phase * 1.19) *
			0.38 +
		std::sin(static_cast<double>(x + z) * 0.014 +
			static_cast<double>(y) * 0.041 + seed_phase) * 0.29;
}

bool underground_ore_patch(
	std::int64_t x,
	std::int64_t y,
	std::int64_t z,
	double seed_phase
) noexcept {
	const double vein =
		std::sin(static_cast<double>(x) * 0.073 + seed_phase) *
		std::cos(static_cast<double>(y) * 0.097 - seed_phase * 0.7) *
		std::sin(static_cast<double>(z) * 0.061 + seed_phase * 1.3);
	const double pocket = procedural_wave(x, y, z, seed_phase * 0.37);
	return vein > 0.48 && pocket > 0.08;
}

double smoothstep(double edge_a, double edge_b, double value) noexcept {
	if (!(edge_b > edge_a)) return value >= edge_b ? 1.0 : 0.0;
	const double t = std::clamp(
		(value - edge_a) / (edge_b - edge_a), 0.0, 1.0
	);
	return t * t * (3.0 - 2.0 * t);
}

double gaussian_feature(
	double x,
	double z,
	double center_x,
	double center_z,
	double radius_x,
	double radius_z,
	double amplitude
) noexcept {
	const double dx = (x - center_x) / radius_x;
	const double dz = (z - center_z) / radius_z;
	const double distance_squared = dx * dx + dz * dz;
	if (distance_squared >= 16.0) return 0.0;
	return amplitude * std::exp(-distance_squared);
}

struct WtProceduralLake {
	double center_x = 0.0;
	double center_z = 0.0;
	double radius_x = 1.0;
	double radius_z = 1.0;
	double water_level = 0.0;
	double depression_depth = 0.0;
};

constexpr std::array<WtProceduralLake, 3> kFourBiomeLakes = {{
	{ 650.0, 700.0, 230.0, 170.0, 23.5, 34.0 },
	{ 1400.0, 700.0, 240.0, 170.0, 21.5, 36.0 },
	{ 650.0, 1370.0, 220.0, 160.0, 27.5, 38.0 },
}};

double four_biome_lake_depression(double x, double z) noexcept {
	double depression = 0.0;
	for (const WtProceduralLake &lake : kFourBiomeLakes) {
		const double dx = (x - lake.center_x) / lake.radius_x;
		const double dz = (z - lake.center_z) / lake.radius_z;
		const double q_squared = dx * dx + dz * dz;
		if (q_squared >= 1.0) continue;
		const double q = std::sqrt(q_squared);
		const double falloff = 1.0 - smoothstep(0.15, 1.0, q);
		depression = std::max(
			depression,
			lake.depression_depth * falloff
		);
	}
	return depression;
}

double four_biome_static_water_density(
	double x,
	double y,
	double z
) noexcept {
	double density = std::numeric_limits<double>::infinity();
	for (const WtProceduralLake &lake : kFourBiomeLakes) {
		const double dx = (x - lake.center_x) / lake.radius_x;
		const double dz = (z - lake.center_z) / lake.radius_z;
		const double radial_distance =
			(std::sqrt(dx * dx + dz * dz) - 1.0) *
			std::min(lake.radius_x, lake.radius_z);
		density = std::min(
			density,
			std::max(y - lake.water_level, radial_distance)
		);
	}
	return density;
}

class WtProceduralTerrainVolumeSource final : public WtChunkSampleSource {
public:
	explicit WtProceduralTerrainVolumeSource(
		WtProceduralWorldDescriptor descriptor
	) noexcept :
			descriptor_(descriptor) {
		if (has_reference_roads()) {
			const auto &segments = wt_reference_road_segments();
			for (std::size_t index = 0; index < segments.size(); ++index) {
				const WtReferenceRoadSegment &segment = segments[index];
				road_height_a_[index] = rolling_hills_height(
					static_cast<std::int64_t>(segment.ax), static_cast<std::int64_t>(segment.az)
				);
				road_height_b_[index] = rolling_hills_height(
					static_cast<std::int64_t>(segment.bx), static_cast<std::int64_t>(segment.bz)
				);
			}
		}
		if (has_expansive_roads()) {
			const auto &segments = wt_expansive_road_segments();
			for (std::size_t index = 0; index < segments.size(); ++index) {
				const WtReferenceRoadSegment &segment = segments[index];
				expansive_road_height_a_[index] = four_biome_height(
					static_cast<std::int64_t>(segment.ax),
					static_cast<std::int64_t>(segment.az)
				);
				expansive_road_height_b_[index] = four_biome_height(
					static_cast<std::int64_t>(segment.bx),
					static_cast<std::int64_t>(segment.bz)
				);
			}
			four_biome_cave_portal_surface_y_[0] = four_biome_height(360, 520);
			four_biome_cave_portal_surface_y_[1] = four_biome_height(1570, 520);
			four_biome_cave_portal_surface_y_[2] = four_biome_height(430, 1540);
		}
	}

	bool sample(
		const WtGridPoint &point,
		WtScalarSample &output
	) const noexcept override {
		const double base_surface = height(point.x, point.z);
		WtProceduralRoadField road{
			base_surface, std::numeric_limits<double>::infinity()
		};
		if (has_reference_roads()) {
			road = wt_sample_reference_road_field(
				base_surface,
				static_cast<double>(point.x),
				static_cast<double>(point.z),
				road_height_a_,
				road_height_b_
			);
		} else if (has_expansive_roads()) {
			road = wt_sample_expansive_road_field(
				base_surface,
				static_cast<double>(point.x),
				static_cast<double>(point.z),
				expansive_road_height_a_,
				expansive_road_height_b_
			);
		}
		const double surface = road.surface;
		double density = static_cast<double>(point.y) - surface;
		if (has_rolling_hills_cave()) {
			density = wt_apply_reference_cave_density(
				density,
				static_cast<double>(point.x),
				static_cast<double>(point.y),
				static_cast<double>(point.z)
			);
		} else if (is_four_biome_world()) {
			density = wt_apply_four_biome_cave_density(
				density,
				static_cast<double>(point.x),
				static_cast<double>(point.y),
				static_cast<double>(point.z),
				four_biome_cave_portal_surface_y_
			);
		}
		output.density = regularized_density(density);
		output.static_water_density = is_four_biome_world() ?
			regularized_density(four_biome_static_water_density(
				static_cast<double>(point.x),
				static_cast<double>(point.y),
				static_cast<double>(point.z)
			)) : kWtNoStaticWaterDensity;
		output.material = is_four_biome_world() && density >= 0.0 ? 1 :
			material(
				surface,
				road.distance,
				point.x,
				point.y,
				point.z
			);
		if (is_four_biome_world() && density >= 0.0 &&
			output.static_water_density < 0.0F) {
			output.material = 9;
		}
		output.material_authored = false;
		return std::isfinite(output.density);
	}

private:
	bool has_rolling_hills_cave() const noexcept {
		return descriptor_.mode == WtProceduralWorldMode::RollingHillsCave ||
			descriptor_.mode == WtProceduralWorldMode::RollingHillsCaveRoads;
	}

	bool has_reference_roads() const noexcept {
		return descriptor_.mode == WtProceduralWorldMode::RollingHillsCaveRoads;
	}

	bool is_four_biome_world() const noexcept {
		return descriptor_.mode ==
			WtProceduralWorldMode::FourBiomesLakesCavesRoads;
	}

	bool has_expansive_roads() const noexcept {
		return is_four_biome_world();
	}

	double height(std::int64_t x, std::int64_t z) const noexcept {
		if (descriptor_.mode == WtProceduralWorldMode::Flat) {
			return 8.0;
		}
		if (has_rolling_hills_cave()) {
			return rolling_hills_height(x, z);
		}
		if (is_four_biome_world()) {
			return four_biome_height(x, z);
		}
		const double width = std::max(
			16.0,
			static_cast<double>(descriptor_.chunk_count_x) *
				static_cast<double>(kWtChunkCellsPerAxis)
		);
		const double depth = std::max(
			16.0,
			static_cast<double>(descriptor_.chunk_count_z) *
				static_cast<double>(kWtChunkCellsPerAxis)
		);
		const double center_x = width * 0.5 - 0.5;
		const double center_z = depth * 0.5 - 0.5;
		const double normalized_x =
			(static_cast<double>(x) - center_x) / std::max(center_x, 1.0);
		const double normalized_z =
			(static_cast<double>(z) - center_z) / std::max(center_z, 1.0);
		const double phase =
			static_cast<double>(descriptor_.seed % 100000U) * 0.0001;
		const double radial_distance =
			normalized_x * normalized_x + normalized_z * normalized_z;
		const double central_highland = 18.0 * std::exp(
			-2.1 * radial_distance
		);
		const double ridge_axis = normalized_z + normalized_x * 0.34 - 0.06;
		const double ridge_along = normalized_x - 0.05;
		const double mountain_range = 18.0 *
			std::exp(-48.0 * ridge_axis * ridge_axis) *
			std::exp(-1.2 * ridge_along * ridge_along);
		const double spire_a = 50.0 * std::exp(
			-165.0 * (
				(normalized_x - 0.16) * (normalized_x - 0.16) +
				(normalized_z + 0.02) * (normalized_z + 0.02)
			)
		);
		const double spire_b = 38.0 * std::exp(
			-170.0 * (
				(normalized_x + 0.16) * (normalized_x + 0.16) +
				(normalized_z - 0.19) * (normalized_z - 0.19)
			)
		);
		const double spire_c = 34.0 * std::exp(
			-190.0 * (
				(normalized_x - 0.36) * (normalized_x - 0.36) +
				(normalized_z + 0.30) * (normalized_z + 0.30)
			)
		);
		const double knife_ridge = 20.0 *
			std::exp(-95.0 * (
				normalized_z + 0.22 * normalized_x + 0.13
			) * (
				normalized_z + 0.22 * normalized_x + 0.13
			)) *
			std::exp(-3.0 * (normalized_x - 0.25) * (normalized_x - 0.25));
		const double cliff = 10.0 /
			(1.0 + std::exp(-35.0 * (
				0.20 - normalized_z + 0.18 * normalized_x
			))) *
			std::exp(-1.8 * (normalized_x - 0.18) * (normalized_x - 0.18));
		const double basin = -12.0 * std::exp(
			-5.0 * (
				(normalized_x + 0.44) * (normalized_x + 0.44) +
				(normalized_z - 0.27) * (normalized_z - 0.27)
			)
		);
		const double macro =
			4.0 * std::sin(static_cast<double>(x) * 0.0032 + phase * 1.7) +
			3.2 * std::cos(static_cast<double>(z) * 0.0038 - phase * 1.3) +
			2.3 * std::sin(static_cast<double>(x + z) * 0.0024 + phase);
		const double hills =
			2.6 * std::sin(static_cast<double>(x) * 0.010 + phase) *
				std::cos(static_cast<double>(z) * 0.0085 - phase * 0.5) +
			1.6 * std::cos(static_cast<double>(x - z) * 0.0075 - phase * 0.25);
		const double crag =
			std::sin(static_cast<double>(x) * 0.045 + phase) *
			std::cos(static_cast<double>(z) * 0.041 - phase * 0.5);
		const double crags = 6.0 * std::max(0.0, crag) *
			std::max(0.0, crag) * std::exp(-2.1 * radial_distance);
		const double long_wave =
			1.0 * std::sin(static_cast<double>(x) * 0.016 + phase) +
			0.8 * std::cos(static_cast<double>(z) * 0.014 - phase);
		const double local = 0.45 * std::cos(
			static_cast<double>(x - z) * 0.021 - phase * 0.25
		);
		return 12.0 + central_highland + mountain_range + spire_a +
			spire_b + spire_c + knife_ridge + cliff + basin + macro +
			hills + crags + long_wave + local;
	}

	double four_biome_height(
		std::int64_t x,
		std::int64_t z
	) const noexcept {
		const double px = static_cast<double>(x);
		const double pz = static_cast<double>(z);
		const double phase =
			static_cast<double>(descriptor_.seed % 100000U) * 0.0001;
		const double broad =
			8.0 * std::sin(px * 0.0031 + phase * 1.4) +
			6.5 * std::cos(pz * 0.0027 - phase * 0.9) +
			4.5 * std::sin((px + pz) * 0.0017 + phase * 0.6);
		const double rolling =
			5.0 * std::sin((px - pz) * 0.0073 + phase) *
				std::cos((px + pz) * 0.0058 - phase * 0.4);
		const double detail =
			2.2 * std::sin(px * 0.021 + phase * 0.7) *
				std::cos(pz * 0.019 - phase) +
			1.4 * std::sin((px + 2.0 * pz) * 0.034 + phase * 0.2) +
			0.8 * std::cos((2.0 * px - pz) * 0.047);
		const double snow_mountains =
			gaussian_feature(px, pz, 1540.0, 1500.0, 230.0, 210.0, 38.0) +
			gaussian_feature(px, pz, 1760.0, 1320.0, 170.0, 200.0, 28.0) +
			gaussian_feature(px, pz, 1280.0, 1730.0, 190.0, 150.0, 26.0);
		const double gravel_highlands =
			gaussian_feature(px, pz, 470.0, 1510.0, 250.0, 230.0, 22.0) +
			gaussian_feature(px, pz, 700.0, 1700.0, 210.0, 180.0, 12.0);
		const double grass_hills =
			gaussian_feature(px, pz, 350.0, 360.0, 280.0, 230.0, 8.0) +
			gaussian_feature(px, pz, 800.0, 300.0, 240.0, 200.0, 5.0);
		return 34.0 + broad + rolling + detail + snow_mountains +
			gravel_highlands + grass_hills -
			four_biome_lake_depression(px, pz);
	}

	double rolling_hills_height(
		std::int64_t x,
		std::int64_t z
	) const noexcept {
		const double width = std::max(
			16.0,
			static_cast<double>(descriptor_.chunk_count_x) *
				static_cast<double>(kWtChunkCellsPerAxis)
		);
		const double depth = std::max(
			16.0,
			static_cast<double>(descriptor_.chunk_count_z) *
				static_cast<double>(kWtChunkCellsPerAxis)
		);
		const double center_x = width * 0.5 - 0.5;
		const double center_z = depth * 0.5 - 0.5;
		const double normalized_x =
			(static_cast<double>(x) - center_x) / std::max(center_x, 1.0);
		const double normalized_z =
			(static_cast<double>(z) - center_z) / std::max(center_z, 1.0);
		const double phase =
			static_cast<double>(descriptor_.seed % 100000U) * 0.0001;
		const double broad =
			7.5 * std::sin(static_cast<double>(x) * 0.0052 + phase * 1.7) +
			5.0 * std::cos(static_cast<double>(z) * 0.0047 - phase * 1.2) +
			3.0 * std::sin(static_cast<double>(x + z) * 0.0034 + phase);
		const double rolling =
			4.0 * std::sin(static_cast<double>(x - z) * 0.008 + phase) *
				std::cos(static_cast<double>(x + z) * 0.006 - phase * 0.6);
		const double central_mound = 8.0 * std::exp(
			-2.7 * (
				(normalized_x - 0.03) * (normalized_x - 0.03) +
				(normalized_z + 0.06) * (normalized_z + 0.06)
			)
		);
		const double shallow_valley = -5.5 * std::exp(
			-5.0 * (
				(normalized_x + 0.34) * (normalized_x + 0.34) +
				(normalized_z - 0.22) * (normalized_z - 0.22)
			)
		);
		const double local =
			1.2 * std::sin(static_cast<double>(x) * 0.018 - phase * 0.5) +
			0.9 * std::cos(static_cast<double>(z) * 0.016 + phase * 0.9);
		return 26.0 + broad + rolling + central_mound + shallow_valley + local;
	}

	std::uint16_t material(
		double surface,
		double road_distance,
		std::int64_t x,
		std::int64_t y,
		std::int64_t z
	) const noexcept {
		const double seed_phase =
			static_cast<double>(descriptor_.seed % 100000U) * 0.0001;
		const double depth = surface - static_cast<double>(y);
		if (has_reference_roads() &&
			wt_reference_road_has_asphalt(road_distance, depth)) {
			return 10;
		}
		if (has_expansive_roads() &&
			wt_expansive_road_has_asphalt(road_distance, depth)) {
			return 10;
		}
		if (depth >= 12.0 && underground_ore_patch(x, y, z, seed_phase)) {
			return 8;
		}
		if (depth >= 8.0) return 1;

		if (is_four_biome_world()) {
			const double width = std::max(
				16.0,
				static_cast<double>(descriptor_.chunk_count_x) *
					static_cast<double>(kWtChunkCellsPerAxis)
			);
			const double world_depth = std::max(
				16.0,
				static_cast<double>(descriptor_.chunk_count_z) *
					static_cast<double>(kWtChunkCellsPerAxis)
			);
			const double center_x = width * 0.5 - 0.5;
			const double center_z = world_depth * 0.5 - 0.5;
			const double vertical_boundary = center_x + 110.0 * std::sin(
				(static_cast<double>(z) - center_z) * 0.0024 +
				seed_phase * 0.8
			);
			const double horizontal_boundary = center_z + 90.0 * std::cos(
				(static_cast<double>(x) - center_x) * 0.0022 -
				seed_phase * 0.7
			);
			const bool west = static_cast<double>(x) < vertical_boundary;
			const bool south = static_cast<double>(z) < horizontal_boundary;
			if (west && south) return 2;
			if (!west && south) return 4;
			if (west && !south) return 3;
			return 5;
		}

		const double macro_biome =
			std::sin(static_cast<double>(x) * 0.0042 + seed_phase * 1.5) +
			std::cos(static_cast<double>(z) * 0.0037 - seed_phase * 0.8) +
			0.55 * std::sin(static_cast<double>(x - z) * 0.0021 + seed_phase);
		const double dry_biome =
			std::sin(static_cast<double>(x + z) * 0.0051 - seed_phase * 0.4) +
			0.42 * std::cos(static_cast<double>(x) * 0.0063 + seed_phase);
		if (surface > 40.0 || (surface > 30.0 && macro_biome > 0.35)) {
			return 5;
		}
		if (surface < 10.0 || dry_biome > 0.85) {
			return 4;
		}
		if (surface > 22.0 || macro_biome < -0.70) {
			return 3;
		}
		return 2;
	}

	WtProceduralWorldDescriptor descriptor_;
	std::array<double, kWtReferenceRoadSegmentCount> road_height_a_{}, road_height_b_{};
	std::array<double, kWtExpansiveRoadSegmentCount>
		expansive_road_height_a_{}, expansive_road_height_b_{};
	std::array<double, kWtFourBiomeCaveCount>
		four_biome_cave_portal_surface_y_{};
};

} // namespace

bool wt_sample_procedural_world(
	const WtProceduralWorldDescriptor &descriptor,
	const WtGridPoint &point,
	WtScalarSample &output
) noexcept {
	output = {};
	if (!wt_valid_procedural_descriptor(descriptor)) {
		return false;
	}
	return WtProceduralTerrainVolumeSource(descriptor).sample(point, output);
}

bool wt_valid_procedural_descriptor(
	const WtProceduralWorldDescriptor &descriptor
) noexcept {
	const std::uint64_t page_count = wt_procedural_page_count(descriptor);
	return descriptor.chunk_count_x != 0 &&
		descriptor.chunk_count_y != 0 &&
		descriptor.chunk_count_z != 0 &&
		descriptor.source_revision != 0 &&
		page_count != 0 &&
		page_count <= kWtMaximumProceduralPageCount;
}

std::uint64_t wt_procedural_page_count(
	const WtProceduralWorldDescriptor &descriptor
) noexcept {
	if (
		descriptor.chunk_count_x == 0 ||
		descriptor.chunk_count_y == 0 ||
		descriptor.chunk_count_z == 0
	) {
		return 0;
	}
	std::uint64_t pages = 0;
	for (std::uint8_t lod = 0; lod <= kWtProceduralMaximumLod; ++lod) {
		const std::uint32_t span = lod_span(lod);
		pages += static_cast<std::uint64_t>(
			ceil_divide_u32(descriptor.chunk_count_x, span)
		) * static_cast<std::uint64_t>(
			ceil_divide_u32(descriptor.chunk_count_z, span)
		) * static_cast<std::uint64_t>(
			vertical_layer_count(descriptor, lod)
		);
	}
	return pages;
}

std::vector<WtChunkKey> wt_procedural_keys(
	const WtProceduralWorldDescriptor &descriptor
) {
	std::vector<WtChunkKey> keys;
	keys.reserve(static_cast<std::size_t>(wt_procedural_page_count(descriptor)));
	for (std::uint8_t lod = 0; lod <= kWtProceduralMaximumLod; ++lod) {
		const std::uint32_t span = lod_span(lod);
		const std::uint32_t count_x = ceil_divide_u32(descriptor.chunk_count_x, span);
		const std::uint32_t count_z = ceil_divide_u32(descriptor.chunk_count_z, span);
		const std::uint32_t count_y = vertical_layer_count(descriptor, lod);
		const std::int32_t origin_y = vertical_origin(descriptor, lod);
		for (std::uint32_t z = 0; z < count_z; ++z) {
			for (std::uint32_t y = 0; y < count_y; ++y) {
				for (std::uint32_t x = 0; x < count_x; ++x) {
					keys.push_back({
						static_cast<std::int32_t>(x),
						static_cast<std::int32_t>(origin_y + static_cast<std::int32_t>(y)),
						static_cast<std::int32_t>(z),
						lod,
					});
				}
			}
		}
	}
	std::sort(keys.begin(), keys.end());
	return keys;
}

bool wt_procedural_has_key(
	const std::vector<WtChunkKey> &keys,
	const WtChunkKey &key
) noexcept {
	return std::binary_search(keys.begin(), keys.end(), key);
}

bool wt_procedural_can_generate_page(
	const WtProceduralWorldDescriptor &descriptor,
	const WtChunkKey &key
) noexcept {
	if (!wt_is_valid_chunk_key(key) || key.lod > kWtProceduralMaximumLod) {
		return false;
	}
	const std::uint32_t span = lod_span(key.lod);
	const std::int32_t count_x = static_cast<std::int32_t>(
		ceil_divide_u32(descriptor.chunk_count_x, span)
	);
	const std::int32_t count_z = static_cast<std::int32_t>(
		ceil_divide_u32(descriptor.chunk_count_z, span)
	);
	const std::int32_t min_y = vertical_origin(descriptor, key.lod);
	const std::int32_t max_y = static_cast<std::int32_t>(
		min_y + static_cast<std::int32_t>(vertical_layer_count(descriptor, key.lod))
	);
	return key.x >= -1 &&
		key.x <= count_x &&
		key.z >= -1 &&
		key.z <= count_z &&
		key.y >= min_y - 1 &&
		key.y <= max_y;
}

WtPageLoadCompletion wt_generate_procedural_page(
	const WtProceduralWorldDescriptor &descriptor,
	const WtChunkKey &key,
	WtGenerationToken generation,
	std::uint64_t &bytes_read
) {
	WtPageLoadCompletion completion;
	completion.key = key;
	completion.generation = generation;
	if (!wt_procedural_can_generate_page(descriptor, key)) {
		completion.status = WtPageLoadStatus::PageFailure;
		return completion;
	}
	WtProceduralTerrainVolumeSource source(descriptor);
	std::vector<WtBakedChunkPage> pages;
	const WtChunkBaker baker(1);
	if (baker.bake({ key }, descriptor.source_revision, source, pages) !=
			WtChunkBakeStatus::Ok ||
		pages.size() != 1) {
		completion.status = WtPageLoadStatus::PageFailure;
		return completion;
	}
	auto bytes = std::make_shared<std::vector<std::uint8_t>>(
		std::move(pages[0].bytes)
	);
	bytes_read = bytes->size();
	completion.status = WtPageLoadStatus::Ok;
	completion.page_bytes = std::move(bytes);
	return completion;
}

} // namespace world_transvoxel
