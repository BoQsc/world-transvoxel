#pragma once

#include "core/wt_chunk_key.h"
#include "core/wt_chunk_state.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace world_transvoxel {

struct WtChunkPublicationRegion {
	std::vector<WtChunkKey> replacements;
	std::vector<WtChunkKey> retirements;
};

struct WtGpuPublicationBoundary {
	std::uint8_t transition_mask = 0;
	bool compatible_active = false;
};

// Inputs are sorted unique keys. Lookup includes desired visual leaves only,
// excluding pending retirements.
// A successful build may still need newer masks or unprepared generations.
bool wt_build_gpu_chunk_publication_cohort(
	const WtChunkKey &seed,
	const std::vector<WtChunkKey> &pending_replacements,
	const std::vector<WtChunkKey> &pending_retirements,
	const std::function<bool(const WtChunkKey &, WtGpuPublicationBoundary &)> &lookup,
	WtChunkPublicationRegion &output,
	std::vector<WtChunkKey> &waiting_masks,
	std::size_t maximum_members = 4096
);

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

bool wt_chunk_publication_region_has_complete_authoritative_coverage(
	const WtChunkPublicationRegion &region,
	const std::function<bool(const WtChunkKey &)> &is_authoritative
);

bool wt_collision_retirement_is_safe(
	const WtChunkKey &retirement,
	const std::vector<WtChunkKey> &required_collision_chunks,
	const std::vector<WtChunkKey> &physically_ready_collision_chunks
) noexcept;

bool wt_required_collision_can_publish_independently(
	WtGenerationToken record_generation,
	WtGenerationToken render_generation,
	WtGenerationToken collision_generation,
	WtGenerationToken staged_collision_generation,
	bool collision_required,
	bool collision_ready,
	bool visual_required
) noexcept;

} // namespace world_transvoxel
