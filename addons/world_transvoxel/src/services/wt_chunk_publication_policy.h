#pragma once

#include "core/wt_chunk_key.h"

#include <vector>

namespace world_transvoxel {

struct WtChunkPublicationRegion {
	std::vector<WtChunkKey> replacements;
	std::vector<WtChunkKey> retirements;
};

bool wt_chunk_replacement_requires_regional_publication(
	const WtChunkKey &replacement,
	const std::vector<WtChunkKey> &pending_retirements
) noexcept;

bool wt_build_chunk_publication_region(
	const WtChunkKey &seed_replacement,
	const std::vector<WtChunkKey> &pending_replacements,
	const std::vector<WtChunkKey> &pending_retirements,
	WtChunkPublicationRegion &output
);

bool wt_chunk_publication_region_has_complete_coverage(
	const WtChunkPublicationRegion &region
) noexcept;

} // namespace world_transvoxel
