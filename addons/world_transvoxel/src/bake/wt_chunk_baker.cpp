#include "bake/wt_chunk_baker.h"

#include "meshing/wt_multiresolution_vertex_resolver.h"
#include "storage/wt_chunk_surface_shift.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace world_transvoxel {

WtSurfaceShiftBuildStatus wt_build_surface_shift_records(
	WtChunkPage &page,
	const WtChunkSampleSource &source,
	WtMultiresolutionVertexScratch &scratch
) {
	page.surface_shift_isovalue = 0.0F;
	page.surface_shift_records.clear();
	page.surface_shift_valid = false;
	if (!wt_is_valid_chunk_key(page.metadata.key) ||
		page.metadata.cell_spacing != static_cast<std::uint64_t>(
			wt_lod_cell_size(page.metadata.key.lod)
		) ||
		page.samples.size() != kWtChunkPageSampleCount) {
		return WtSurfaceShiftBuildStatus::InvalidPage;
	}
	if (page.metadata.key.lod == 0) {
		page.surface_shift_valid = true;
		return WtSurfaceShiftBuildStatus::Ok;
	}
	scratch.reset();
	page.surface_shift_records.reserve(kWtChunkSurfaceEdgeCount / 8);
	for (std::size_t edge = 0;
		edge < kWtChunkSurfaceEdgeCount;
		++edge) {
		WtGridPoint edge_a;
		WtGridPoint edge_b;
		WtScalarSample coarse_a;
		WtScalarSample coarse_b;
		if (!wt_chunk_surface_edge_points(
				page.metadata,
				static_cast<std::uint16_t>(edge),
				edge_a,
				edge_b
			) ||
			!wt_sample_chunk_page(page, edge_a, coarse_a) ||
			!wt_sample_chunk_page(page, edge_b, coarse_b)) {
			page.surface_shift_records.clear();
			return WtSurfaceShiftBuildStatus::InvalidPage;
		}
		if ((coarse_a.density < page.surface_shift_isovalue) ==
			(coarse_b.density < page.surface_shift_isovalue)) {
			continue;
		}
		WtResolvedMultiresolutionEdge resolved;
		if (wt_resolve_multiresolution_edge(
				edge_a,
				edge_b,
				page.surface_shift_isovalue,
				source,
				scratch,
				resolved
			) != WtMultiresolutionVertexStatus::Ok) {
			page.surface_shift_records.clear();
			return WtSurfaceShiftBuildStatus::SampleSourceFailure;
		}
		std::int64_t offset = 0;
		bool valid_resolved_edge = false;
		if (edge_a.x != edge_b.x) {
			offset = resolved.endpoint_a.x - edge_a.x;
			valid_resolved_edge = resolved.endpoint_b.x ==
				resolved.endpoint_a.x + 1 &&
				resolved.endpoint_a.y == edge_a.y &&
				resolved.endpoint_a.z == edge_a.z;
		} else if (edge_a.y != edge_b.y) {
			offset = resolved.endpoint_a.y - edge_a.y;
			valid_resolved_edge = resolved.endpoint_b.y ==
				resolved.endpoint_a.y + 1 &&
				resolved.endpoint_a.x == edge_a.x &&
				resolved.endpoint_a.z == edge_a.z;
		} else {
			offset = resolved.endpoint_a.z - edge_a.z;
			valid_resolved_edge = resolved.endpoint_b.z ==
				resolved.endpoint_a.z + 1 &&
				resolved.endpoint_a.x == edge_a.x &&
				resolved.endpoint_a.y == edge_a.y;
		}
		if (!valid_resolved_edge || offset < 0 ||
			static_cast<std::uint64_t>(offset) >=
				page.metadata.cell_spacing) {
			page.surface_shift_records.clear();
			return WtSurfaceShiftBuildStatus::SampleSourceFailure;
		}
		page.surface_shift_records.push_back({
			static_cast<std::uint16_t>(edge),
			static_cast<std::uint32_t>(offset),
			resolved.sample_a,
			resolved.sample_b,
		});
	}
	page.surface_shift_valid = true;
	return WtSurfaceShiftBuildStatus::Ok;
}

WtChunkBaker::WtChunkBaker(std::size_t page_capacity) noexcept :
		page_capacity_(page_capacity) {
}

WtChunkBakeStatus WtChunkBaker::bake(
	const std::vector<WtChunkKey> &keys,
	std::uint64_t source_revision,
	const WtChunkSampleSource &source,
	std::vector<WtBakedChunkPage> &output
) const {
	output.clear();
	if (keys.size() > page_capacity_) {
		return WtChunkBakeStatus::PageCapacityExceeded;
	}
	std::vector<WtChunkKey> ordered = keys;
	std::sort(ordered.begin(), ordered.end());
	for (std::size_t index = 0; index < ordered.size(); ++index) {
		if (!wt_is_valid_chunk_key(ordered[index])) {
			return WtChunkBakeStatus::InvalidInput;
		}
		if (index != 0 && ordered[index - 1] == ordered[index]) {
			return WtChunkBakeStatus::DuplicateKey;
		}
	}

	output.reserve(ordered.size());
	WtMultiresolutionVertexScratch surface_shift_scratch;
	for (const WtChunkKey &key : ordered) {
		WtChunkPage page;
		page.metadata.key = key;
		page.metadata.cell_spacing = static_cast<std::uint64_t>(
			wt_lod_cell_size(key.lod)
		);
		page.metadata.source_revision = source_revision;
		page.samples.reserve(kWtChunkPageSampleCount);
		const WtChunkBounds bounds = wt_chunk_bounds(key);
		const std::int64_t spacing = wt_lod_cell_size(key.lod);
		for (int z = -1; z <= 17; ++z) {
			for (int y = -1; y <= 17; ++y) {
				for (int x = -1; x <= 17; ++x) {
					const WtGridPoint point = {
						bounds.minimum.x + static_cast<std::int64_t>(x) * spacing,
						bounds.minimum.y + static_cast<std::int64_t>(y) * spacing,
						bounds.minimum.z + static_cast<std::int64_t>(z) * spacing,
					};
					WtScalarSample sample;
					if (!source.sample(point, sample) || !std::isfinite(sample.density)) {
						output.clear();
						return WtChunkBakeStatus::SampleSourceFailure;
					}
					page.samples.push_back(sample);
				}
			}
		}
		if (wt_build_surface_shift_records(
				page, source, surface_shift_scratch
			) != WtSurfaceShiftBuildStatus::Ok) {
			output.clear();
			return WtChunkBakeStatus::SampleSourceFailure;
		}
		WtBakedChunkPage baked;
		baked.key = key;
		if (wt_write_chunk_page(page, baked.bytes) != WtChunkPageStatus::Ok) {
			output.clear();
			return WtChunkBakeStatus::PageWriteFailure;
		}
		baked.content_hash = wt_sha256(baked.bytes.data(), baked.bytes.size());
		output.push_back(std::move(baked));
	}
	return WtChunkBakeStatus::Ok;
}

} // namespace world_transvoxel
