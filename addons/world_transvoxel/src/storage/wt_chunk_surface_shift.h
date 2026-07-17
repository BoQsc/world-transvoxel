#pragma once

#include "storage/wt_chunk_page.h"

#include <cstdint>

namespace world_transvoxel {

bool wt_chunk_surface_edge_index(
	const WtChunkPageMetadata &metadata,
	const WtGridPoint &endpoint_a,
	const WtGridPoint &endpoint_b,
	std::uint16_t &edge_index,
	bool &reversed
) noexcept;

bool wt_chunk_surface_edge_points(
	const WtChunkPageMetadata &metadata,
	std::uint16_t edge_index,
	WtGridPoint &endpoint_a,
	WtGridPoint &endpoint_b
) noexcept;

bool wt_resolve_chunk_surface_shift_record(
	const WtChunkPage &page,
	const WtGridPoint &endpoint_a,
	const WtGridPoint &endpoint_b,
	float isovalue,
	WtResolvedMultiresolutionEdge &output
) noexcept;

} // namespace world_transvoxel
