#pragma once

#include <array>

namespace world_transvoxel {

struct WtProceduralCaveField {
	double distance = 0.0;
	double air_density = 0.0;
};

WtProceduralCaveField wt_sample_reference_cave_field(
	double x,
	double y,
	double z
) noexcept;

double wt_apply_reference_cave_density(
	double terrain_density,
	double x,
	double y,
	double z
) noexcept;

constexpr std::size_t kWtFourBiomeCaveCount = 3;

WtProceduralCaveField wt_sample_four_biome_cave_field(
	double x,
	double y,
	double z,
	const std::array<double, kWtFourBiomeCaveCount> &portal_surface_y
) noexcept;

double wt_apply_four_biome_cave_density(
	double terrain_density,
	double x,
	double y,
	double z,
	const std::array<double, kWtFourBiomeCaveCount> &portal_surface_y
) noexcept;

} // namespace world_transvoxel
