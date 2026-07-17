#include "meshing/wt_material_volume_sample_source.h"

#include "meshing/wt_multiresolution_vertex_resolver.h"

#include <algorithm>
#include <cmath>

namespace world_transvoxel {
namespace {

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
	WtScalarSample source_sample;
	if (!source_.sample(point, source_sample)) {
		return false;
	}
	// Preserve the terrain field's continuous distance magnitude so water-wall
	// intersections use the same sub-voxel edge positions as terrain. Only the
	// sign is replaced by the material-volume interior classification.
	const float magnitude = std::clamp(
		std::abs(source_sample.density), 0.01F, 1.0F
	);
	output.density = is_surface_interior(point, source_sample) ?
		-magnitude : magnitude;
	output.material = material_;
	output.material_authored = source_sample.material_authored;
	return true;
}

bool WtMaterialVolumeSampleSource::is_surface_interior(
	const WtGridPoint &point,
	const WtScalarSample &sample
) const noexcept {
	if (is_occupied(sample, material_, solid_isovalue_)) {
		return true;
	}
	if (sample.density < solid_isovalue_) {
		return true;
	}
	// Static gravity water renders its free surface, not a closed categorical
	// shell. Air is outside only when the same sampled column contains water
	// below it. Other air and all solid remain inside; vertical closure faces
	// are removed later by the upward-facing free-surface filter.
	for (std::int64_t step = 1; step <= 256; step *= 2) {
		WtScalarSample first_below;
		if (!source_.sample(
				{ point.x, point.y - step, point.z },
				first_below
			)) {
			continue;
		}
		for (std::int64_t offset = step; offset <= 32 * step; offset += step) {
			WtScalarSample below;
			if (!source_.sample(
					{ point.x, point.y - offset, point.z },
					below
				)) {
				break;
			}
			if (is_occupied(below, material_, solid_isovalue_)) {
				return false;
			}
		}
		return true;
	}
	return true;
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
