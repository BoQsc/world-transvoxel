#include "api/world_transvoxel_terrain.h"

#include "physics/wt_godot_collision_sink.h"
#include "render/wt_godot_render_sink.h"
#include "services/wt_chunk_application.h"
#include "services/wt_chunk_publication_policy.h"

#include <algorithm>

namespace world_transvoxel {

void WorldTransvoxelTerrain::flush_ready_independent_publication_regions() {
	if (open_viewer_plan_publications_ != 0) return;
	for (std::size_t index = 0;
			index < independently_publishable_chunk_replacements_.size();) {
		const WtChunkKey seed =
			independently_publishable_chunk_replacements_[index];
		const bool seed_pending = std::binary_search(
			pending_chunk_replacements_.begin(),
			pending_chunk_replacements_.end(),
			seed
		);
		const bool seed_ready = std::binary_search(
			ready_staged_chunk_replacements_.begin(),
			ready_staged_chunk_replacements_.end(),
			seed
		);
		if (!seed_pending && !seed_ready) {
			independently_publishable_chunk_replacements_.erase(
				independently_publishable_chunk_replacements_.begin() + index
			);
			continue;
		}
		if (!wt_chunk_replacement_requires_regional_publication(
				seed,
				pending_chunk_retirements_
			)) {
			++index;
			continue;
		}
		std::vector<WtChunkKey> regional_replacement_candidates =
			pending_chunk_replacements_;
		regional_replacement_candidates.insert(
			regional_replacement_candidates.end(),
			ready_staged_chunk_replacements_.begin(),
			ready_staged_chunk_replacements_.end()
		);
		std::sort(
			regional_replacement_candidates.begin(),
			regional_replacement_candidates.end()
		);
		regional_replacement_candidates.erase(
			std::unique(
				regional_replacement_candidates.begin(),
				regional_replacement_candidates.end()
			),
			regional_replacement_candidates.end()
		);
		WtChunkPublicationRegion region;
		if (!wt_build_chunk_publication_region(
				seed,
				regional_replacement_candidates,
				pending_chunk_retirements_,
				region
			) || !wt_chunk_publication_region_has_complete_coverage(region)) {
			++index;
			continue;
		}
		bool ready = true;
		for (const WtChunkKey &replacement : region.replacements) {
			WtChunkApplicationRecord record;
			if (!application_->copy_record(replacement, record) ||
					!record.fully_ready() ||
					!render_sink_->can_publish_staged_record(
						replacement,
						record.generation
					) ||
					!collision_sink_->can_publish_staged_record(
						replacement,
						record.generation
					)) {
				ready = false;
				break;
			}
		}
		if (!ready) {
			++index;
			continue;
		}
		for (const WtChunkKey &retirement : region.retirements) {
			application_->forget_chunk(retirement);
			render_sink_->begin_render_retirement(retirement);
			collision_sink_->remove_collision(retirement);
			render_sink_->publish_staged_record(retirement);
		}
		bool published = true;
		for (const WtChunkKey &replacement : region.replacements) {
			published = render_sink_->publish_staged_record(replacement) &&
				collision_sink_->publish_staged_record(replacement) && published;
		}
		if (!published) {
			synchronous_world_error_ =
				"regional visibility publication failed";
			return;
		}
		for (const WtChunkKey &retirement : region.retirements) {
			const auto position = std::lower_bound(
				pending_chunk_retirements_.begin(),
				pending_chunk_retirements_.end(),
				retirement
			);
			if (position != pending_chunk_retirements_.end() &&
					*position == retirement) {
				pending_chunk_retirements_.erase(position);
			}
		}
		for (const WtChunkKey &replacement : region.replacements) {
			const auto pending = std::lower_bound(
				pending_chunk_replacements_.begin(),
				pending_chunk_replacements_.end(),
				replacement
			);
			if (pending != pending_chunk_replacements_.end() &&
					*pending == replacement) {
				pending_chunk_replacements_.erase(pending);
			}
			const auto independent = std::lower_bound(
				independently_publishable_chunk_replacements_.begin(),
				independently_publishable_chunk_replacements_.end(),
				replacement
			);
			if (independent !=
					independently_publishable_chunk_replacements_.end() &&
					*independent == replacement) {
				independently_publishable_chunk_replacements_.erase(independent);
			}
			const auto ready_position = std::lower_bound(
				ready_staged_chunk_replacements_.begin(),
				ready_staged_chunk_replacements_.end(),
				replacement
			);
			if (ready_position != ready_staged_chunk_replacements_.end() &&
					*ready_position == replacement) {
				ready_staged_chunk_replacements_.erase(ready_position);
			}
		}
		++regional_visibility_publications_;
		regional_visibility_replacements_ += region.replacements.size();
		regional_visibility_retirements_ += region.retirements.size();
		index = 0;
	}
}

} // namespace world_transvoxel
