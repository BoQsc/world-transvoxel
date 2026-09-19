#pragma once

#include "core/wt_chunk_key.h"
#include "core/wt_chunk_state.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace world_transvoxel {

class WtPublicationDependencyGraph;

struct WtChunkPublicationRegion {
	std::vector<WtChunkKey> replacements;
	std::vector<WtChunkKey> retirements;
};

struct WtGpuPublicationBoundary {
	std::uint8_t transition_mask = 0;
	bool compatible_active = false;
	// False only when this candidate replaces density changed by the current
	// edit. Same-LOD edit neighbors must publish together even with mask zero.
	bool content_current = true;
};

struct WtGpuPublicationCohortDiagnostics {
	std::size_t candidate_count = 0;
	std::size_t selected_member_count = 0;
	std::size_t overlap_members = 0;
	std::size_t same_lod_face_members = 0;
	std::size_t coarse_face_members = 0;
	std::size_t fine_face_members = 0;
	// Zero means no structural blocker. Nonzero values are stable internal
	// diagnostics consumed by the GPU publication bridge.
	std::uint8_t blocker_reason = 0;
	WtChunkKey blocker_key;
	// First stable transition relationship that prevents publication.
	std::uint8_t mask_conflict_reason = 0;
	std::uint8_t mask_conflict_face = 0;
	WtChunkKey mask_conflict_key;
	WtChunkKey mask_conflict_neighbor;
};

WtGpuPublicationBoundary wt_gpu_publication_boundary(
	std::uint8_t candidate_mask, bool candidate_mask_known,
	std::uint8_t active_mask, bool active_present,
	bool active_content_current = true
) noexcept;

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
	std::size_t maximum_members = 4096,
	WtPublicationDependencyGraph *dependencies = nullptr,
	WtGpuPublicationCohortDiagnostics *diagnostics = nullptr
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

// Adds current desired GPU leaves which remain visible and cover retirement
// volume pulled into a cohort solely by unsafe face closure.
void wt_chunk_publication_region_append_retained_coverage(
	WtChunkPublicationRegion &region,
	const std::vector<WtChunkKey> &retained_coverage
);

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
	bool visual_required,
	bool independently_publishable_replacement
) noexcept;

} // namespace world_transvoxel
