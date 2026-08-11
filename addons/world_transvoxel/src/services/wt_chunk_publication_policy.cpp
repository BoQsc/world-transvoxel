#include "services/wt_chunk_publication_policy.h"

namespace world_transvoxel {

bool wt_chunk_replacement_requires_regional_publication(
	const WtChunkKey &replacement,
	const std::vector<WtChunkKey> &pending_retirements
) noexcept {
	if (!wt_is_valid_chunk_key(replacement)) return false;
	const WtChunkBounds replacement_bounds = wt_chunk_bounds(replacement);
	for (const WtChunkKey &retirement : pending_retirements) {
		if (!wt_is_valid_chunk_key(retirement)) continue;
		const WtChunkBounds retirement_bounds = wt_chunk_bounds(retirement);
		const bool overlaps =
			replacement_bounds.minimum.x < retirement_bounds.maximum.x &&
			retirement_bounds.minimum.x < replacement_bounds.maximum.x &&
			replacement_bounds.minimum.y < retirement_bounds.maximum.y &&
			retirement_bounds.minimum.y < replacement_bounds.maximum.y &&
			replacement_bounds.minimum.z < retirement_bounds.maximum.z &&
			retirement_bounds.minimum.z < replacement_bounds.maximum.z;
		if (overlaps) return true;
	}
	return false;
}

} // namespace world_transvoxel
