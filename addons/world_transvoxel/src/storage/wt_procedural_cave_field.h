#pragma once

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

} // namespace world_transvoxel
