#include "meshing/wt_multiresolution_vertex_resolver.h"

#include "meshing/wt_chunk_mesher.h"

#include <cmath>

namespace world_transvoxel {
namespace {

std::size_t mix_hash(std::size_t hash, std::uint64_t value) noexcept {
	value ^= value >> 30;
	value *= 0xbf58476d1ce4e5b9ULL;
	value ^= value >> 27;
	value *= 0x94d049bb133111ebULL;
	value ^= value >> 31;
	return hash ^ static_cast<std::size_t>(
		value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2)
	);
}

WtMultiresolutionVertexStatus get_scalar_sample(
	const WtGridPoint &point,
	const WtChunkSampleSource &source,
	WtMultiresolutionVertexScratch &scratch,
	WtScalarSample &output
) {
	const auto found = scratch.scalar_samples.find(point);
	if (found != scratch.scalar_samples.end()) {
		output = found->second;
		return WtMultiresolutionVertexStatus::Ok;
	}
	if (scratch.scalar_samples.size() >= kWtMaximumSurfaceShiftScalarSamples) {
		return WtMultiresolutionVertexStatus::SampleCacheOverflow;
	}
	if (!source.sample(point, output) || !std::isfinite(output.density)) {
		return WtMultiresolutionVertexStatus::SampleSourceFailure;
	}
	scratch.scalar_samples.emplace(point, output);
	return WtMultiresolutionVertexStatus::Ok;
}

WtMultiresolutionVertexStatus get_finest_cell_sample(
	const WtGridPoint &point,
	const WtChunkSampleSource &source,
	WtMultiresolutionVertexScratch &scratch,
	WtCellSample &output
) {
	const auto found = scratch.cell_samples.find(point);
	if (found != scratch.cell_samples.end()) {
		output = found->second;
		return WtMultiresolutionVertexStatus::Ok;
	}
	if (scratch.cell_samples.size() >= kWtMaximumCachedCellSamples) {
		return WtMultiresolutionVertexStatus::CellSampleCacheOverflow;
	}
	const WtGridPoint offsets[6] = {
		{ point.x - 1, point.y, point.z },
		{ point.x + 1, point.y, point.z },
		{ point.x, point.y - 1, point.z },
		{ point.x, point.y + 1, point.z },
		{ point.x, point.y, point.z - 1 },
		{ point.x, point.y, point.z + 1 },
	};
	WtScalarSample samples[7];
	WtMultiresolutionVertexStatus status = get_scalar_sample(
		point, source, scratch, samples[0]
	);
	for (unsigned int index = 0;
			status == WtMultiresolutionVertexStatus::Ok && index < 6;
			++index) {
		status = get_scalar_sample(
			offsets[index], source, scratch, samples[index + 1]
		);
	}
	if (status != WtMultiresolutionVertexStatus::Ok) {
		return status;
	}
	output = {
		samples[0].density,
		{
			(samples[2].density - samples[1].density) * 0.5F,
			(samples[4].density - samples[3].density) * 0.5F,
			(samples[6].density - samples[5].density) * 0.5F,
		},
		samples[0].material,
	};
	scratch.cell_samples.emplace(point, output);
	return WtMultiresolutionVertexStatus::Ok;
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

} // namespace

std::size_t WtMultiresolutionGridPointHash::operator()(
	const WtGridPoint &point
) const noexcept {
	std::size_t hash = 0;
	hash = mix_hash(hash, static_cast<std::uint64_t>(point.x));
	hash = mix_hash(hash, static_cast<std::uint64_t>(point.y));
	return mix_hash(hash, static_cast<std::uint64_t>(point.z));
}

WtMultiresolutionVertexScratch::WtMultiresolutionVertexScratch() {
	scalar_samples.reserve(kWtMaximumCachedCellSamples);
	cell_samples.reserve(kWtMaximumCachedCellSamples);
}

void WtMultiresolutionVertexScratch::reset() {
	scalar_samples.clear();
	cell_samples.clear();
}

WtMultiresolutionVertexStatus wt_resolve_multiresolution_edge(
	const WtGridPoint &endpoint_a,
	const WtGridPoint &endpoint_b,
	float isovalue,
	const WtChunkSampleSource &source,
	WtMultiresolutionVertexScratch &scratch,
	WtResolvedMultiresolutionEdge &output
) {
	output = {};
	const WtMultiresolutionEdgeSourceStatus source_status =
		source.resolve_multiresolution_edge(
			endpoint_a, endpoint_b, isovalue, output
		);
	if (source_status == WtMultiresolutionEdgeSourceStatus::Ok) {
		return WtMultiresolutionVertexStatus::Ok;
	}
	if (source_status == WtMultiresolutionEdgeSourceStatus::InvalidEdge) {
		return WtMultiresolutionVertexStatus::InvalidEdge;
	}
	if (source_status == WtMultiresolutionEdgeSourceStatus::SampleFailure) {
		return WtMultiresolutionVertexStatus::SampleSourceFailure;
	}
	unsigned int different_axes = 0;
	std::int64_t length = edge_length(
		endpoint_a, endpoint_b, different_axes
	);
	if (different_axes != 1 || length <= 0 ||
		(length & (length - 1)) != 0 || !std::isfinite(isovalue)) {
		return WtMultiresolutionVertexStatus::InvalidEdge;
	}
	WtGridPoint a = endpoint_a;
	WtGridPoint b = endpoint_b;
	WtScalarSample sample_a;
	WtScalarSample sample_b;
	WtMultiresolutionVertexStatus status = get_scalar_sample(
		a, source, scratch, sample_a
	);
	if (status == WtMultiresolutionVertexStatus::Ok) {
		status = get_scalar_sample(b, source, scratch, sample_b);
	}
	if (status != WtMultiresolutionVertexStatus::Ok) {
		return status;
	}
	if ((sample_a.density < isovalue) ==
		(sample_b.density < isovalue)) {
		return WtMultiresolutionVertexStatus::InvalidEdge;
	}
	while (length > 1) {
		const WtGridPoint midpoint = {
			(a.x + b.x) / 2,
			(a.y + b.y) / 2,
			(a.z + b.z) / 2,
		};
		WtScalarSample midpoint_sample;
		status = get_scalar_sample(
			midpoint, source, scratch, midpoint_sample
		);
		if (status != WtMultiresolutionVertexStatus::Ok) {
			return status;
		}
		if ((sample_a.density < isovalue) ==
			(midpoint_sample.density < isovalue)) {
			a = midpoint;
			sample_a = midpoint_sample;
		} else {
			b = midpoint;
			sample_b = midpoint_sample;
		}
		length /= 2;
	}
	output.endpoint_a = a;
	output.endpoint_b = b;
	status = get_finest_cell_sample(
		a, source, scratch, output.sample_a
	);
	if (status == WtMultiresolutionVertexStatus::Ok) {
		status = get_finest_cell_sample(
			b, source, scratch, output.sample_b
		);
	}
	return status;
}

} // namespace world_transvoxel
