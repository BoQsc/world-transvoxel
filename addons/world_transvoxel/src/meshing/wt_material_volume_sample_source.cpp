#include "meshing/wt_material_volume_sample_source.h"

#include "meshing/wt_multiresolution_vertex_resolver.h"

#include <algorithm>
#include <cmath>

namespace world_transvoxel {
namespace {

constexpr std::int64_t kMaximumStaticWaterColumnSearch = 256;

float suppressed_interior_density(const WtScalarSample &sample) noexcept {
	return -std::clamp(std::abs(sample.density), 0.01F, 1.0F);
}

std::int64_t edge_length(
	const WtGridPoint &a,
	const WtGridPoint &b,
	unsigned int &different_axes
) noexcept {
	different_axes = 0;
	std::int64_t length = 0;
	const std::int64_t differences[3] = {
		b.x - a.x,
		b.y - a.y,
		b.z - a.z,
	};
	for (const std::int64_t difference : differences) {
		if (difference == 0) {
			continue;
		}
		++different_axes;
		length = difference < 0 ? -difference : difference;
	}
	return length;
}

bool cell_sample(
	const WtMaterialVolumeSampleSource &source,
	const WtGridPoint &point,
	std::int64_t step,
	WtCellSample &output
) noexcept {
	const WtGridPoint points[7] = {
		point,
		{ point.x - step, point.y, point.z },
		{ point.x + step, point.y, point.z },
		{ point.x, point.y - step, point.z },
		{ point.x, point.y + step, point.z },
		{ point.x, point.y, point.z - step },
		{ point.x, point.y, point.z + step },
	};
	WtScalarSample samples[7];
	for (unsigned int index = 0; index < 7; ++index) {
		if (!source.sample(points[index], samples[index])) {
			return false;
		}
	}
	output = {
		samples[0].density,
		{
			(samples[2].density - samples[1].density) * 0.5F,
			(samples[4].density - samples[3].density) * 0.5F,
			(samples[6].density - samples[5].density) * 0.5F,
		},
		samples[0].material,
		samples[0].material_authored,
	};
	return true;
}

} // namespace

WtMaterialVolumeSampleSource::WtMaterialVolumeSampleSource(
	const WtChunkSampleSource &source,
	std::uint16_t material,
	float solid_isovalue
) noexcept :
		source_(source),
		material_(material),
		solid_isovalue_(solid_isovalue) {
}

bool WtMaterialVolumeSampleSource::sample(
	const WtGridPoint &point,
	WtScalarSample &output
) const noexcept {
	output = {};
	WtScalarSample source_sample;
	if (!source_.sample(point, source_sample)) {
		return false;
	}
	if (material_ == kWtStaticWaterMaterialId &&
		source_sample.static_water_density != kWtNoStaticWaterDensity) {
		// The page-owned secondary field is the canonical water geometry. Terrain
		// remains a separate depth-occluding surface; intersecting both scalar
		// fields here would make the visible shoreline depend on the terrain LOD.
		output.density = source_sample.static_water_density;
		output.material = material_;
		output.material_authored = source_sample.material_authored;
		return std::isfinite(output.density);
	}
	output.density = free_surface_density(point, source_sample);
	output.material = material_;
	output.material_authored = source_sample.material_authored;
	return true;
}

float WtMaterialVolumeSampleSource::free_surface_density(
	const WtGridPoint &point,
	const WtScalarSample &sample
) const noexcept {
	if (is_occupied(sample, material_, solid_isovalue_)) {
		// Use vertical distance to the first exposed air sample instead of the
		// terrain SDF magnitude. Every occupied sample in the same gravity
		// column therefore describes the same horizontal free-surface plane at
		// every LOD. A solid ceiling suppresses the surface rather than creating
		// a water/terrain interface.
		for (std::int64_t step = 1;
			step <= kMaximumStaticWaterColumnSearch; step *= 2) {
			WtScalarSample first_above;
			if (!source_.sample(
					{ point.x, point.y + step, point.z }, first_above
				)) {
				continue;
			}
			for (std::int64_t offset = step;
				offset <= kMaximumStaticWaterColumnSearch; offset += step) {
				WtScalarSample above;
				if (!source_.sample(
						{ point.x, point.y + offset, point.z }, above
					)) {
					break;
				}
				if (is_occupied(above, material_, solid_isovalue_)) {
					continue;
				}
				if (above.density >= solid_isovalue_) {
					return -static_cast<float>(offset) +
						0.5F * static_cast<float>(step);
				}
				return suppressed_interior_density(sample);
			}
			return suppressed_interior_density(sample);
		}
		return suppressed_interior_density(sample);
	}
	if (sample.density < solid_isovalue_) {
		return suppressed_interior_density(sample);
	}
	// Air is exterior only when occupied water is reachable below it before
	// solid terrain. The returned value is the signed vertical distance to the
	// half-grid free surface. Unrelated air remains interior so the secondary
	// Transvoxel field cannot generate a world-sized shell.
	for (std::int64_t step = 1;
		step <= kMaximumStaticWaterColumnSearch; step *= 2) {
		WtScalarSample first_below;
		if (!source_.sample(
				{ point.x, point.y - step, point.z }, first_below
			)) {
			continue;
		}
		for (std::int64_t offset = step;
			offset <= kMaximumStaticWaterColumnSearch; offset += step) {
			WtScalarSample below;
			if (!source_.sample(
					{ point.x, point.y - offset, point.z }, below
				)) {
				break;
			}
			if (is_occupied(below, material_, solid_isovalue_)) {
				return static_cast<float>(offset) -
					0.5F * static_cast<float>(step);
			}
			if (below.density < solid_isovalue_) {
				break;
			}
		}
		return suppressed_interior_density(sample);
	}
	return suppressed_interior_density(sample);
}

WtMultiresolutionEdgeSourceStatus
WtMaterialVolumeSampleSource::resolve_multiresolution_edge(
	const WtGridPoint &endpoint_a,
	const WtGridPoint &endpoint_b,
	float isovalue,
	WtResolvedMultiresolutionEdge &output
) const noexcept {
	output = {};
	unsigned int different_axes = 0;
	const std::int64_t length = edge_length(
		endpoint_a,
		endpoint_b,
		different_axes
	);
	if (different_axes != 1 || length <= 0 ||
		(length & (length - 1)) != 0 || !std::isfinite(isovalue)) {
		return WtMultiresolutionEdgeSourceStatus::InvalidEdge;
	}
	WtScalarSample sample_a;
	WtScalarSample sample_b;
	if (!sample(endpoint_a, sample_a) || !sample(endpoint_b, sample_b)) {
		return WtMultiresolutionEdgeSourceStatus::SampleFailure;
	}
	if ((sample_a.density < isovalue) == (sample_b.density < isovalue)) {
		return WtMultiresolutionEdgeSourceStatus::InvalidEdge;
	}
	output.endpoint_a = endpoint_a;
	output.endpoint_b = endpoint_b;
	if (!cell_sample(*this, endpoint_a, length, output.sample_a) ||
		!cell_sample(*this, endpoint_b, length, output.sample_b)) {
		return WtMultiresolutionEdgeSourceStatus::SampleFailure;
	}
	return WtMultiresolutionEdgeSourceStatus::Ok;
}

bool WtMaterialVolumeSampleSource::is_occupied(
	const WtScalarSample &sample,
	std::uint16_t material,
	float solid_isovalue
) noexcept {
	return material != 0 && sample.material == material &&
		sample.density >= solid_isovalue;
}

} // namespace world_transvoxel
