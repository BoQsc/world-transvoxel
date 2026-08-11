#pragma once

#include "core/wt_chunk_key.h"

#include <vector>

namespace world_transvoxel {

bool wt_chunk_replacement_requires_regional_publication(
	const WtChunkKey &replacement,
	const std::vector<WtChunkKey> &pending_retirements
) noexcept;

} // namespace world_transvoxel
