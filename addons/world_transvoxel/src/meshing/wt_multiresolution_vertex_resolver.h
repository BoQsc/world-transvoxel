#pragma once

#include "backend/wt_cell_types.h"
#include "core/wt_chunk_key.h"

#include <cstdint>

namespace world_transvoxel {

class WtChunkSampleSource;
struct WtMultiresolutionVertexScratch;

enum class WtMultiresolutionVertexStatus : std::uint8_t {
	Ok,
	InvalidEdge,
	SampleSourceFailure,
	SampleCacheOverflow,
	CellSampleCacheOverflow,
};

struct WtResolvedMultiresolutionEdge {
	WtGridPoint endpoint_a;
	WtGridPoint endpoint_b;
	WtCellSample sample_a;
	WtCellSample sample_b;
};

WtMultiresolutionVertexStatus wt_resolve_multiresolution_edge(
	const WtGridPoint &endpoint_a,
	const WtGridPoint &endpoint_b,
	float isovalue,
	const WtChunkSampleSource &source,
	WtMultiresolutionVertexScratch &scratch,
	WtResolvedMultiresolutionEdge &output
);

} // namespace world_transvoxel
