#include "api/world_transvoxel_terrain.h"

#include "physics/wt_godot_collision_sink.h"
#include "render/wt_godot_render_sink.h"
#include "services/wt_chunk_application.h"
#include "services/wt_chunk_publication_policy.h"

#include <algorithm>

namespace world_transvoxel {

bool WorldTransvoxelTerrain::
publication_region_has_complete_authoritative_coverage(
	const WtChunkPublicationRegion &region
) const {
	if (!lifecycle_) return false;
	WtPageHierarchy hierarchy = lifecycle_->page_hierarchy();
	if (!hierarchy.valid()) return false;
	return wt_chunk_publication_region_has_complete_authoritative_coverage(
		region,
		[&hierarchy](const WtChunkKey &key) { return hierarchy.contains(key); }
	);
}

void WorldTransvoxelTerrain::publish_ready_independent_collision_coverage() {
	for (const WtChunkApplicationRecord &record : application_->get_records()) {
		if (!wt_required_collision_can_publish_independently(
				record.generation,
				render_sink_->applied_generation(record.key),
				collision_sink_->applied_generation(record.key),
				collision_sink_->staged_generation(record.key),
				record.collision_required,
				record.collision_ready,
				record.visual_required
			)) {
			continue;
		}
		if (!collision_sink_->publish_staged_record(record.key)) {
			synchronous_world_error_ =
				"independent collision coverage publication failed";
			return;
		}
	}
}

void WorldTransvoxelTerrain::stage_collision_retirement(
	const WtChunkKey &key
) {
	const auto iterator = std::lower_bound(
		pending_collision_retirements_.begin(),
		pending_collision_retirements_.end(),
		key
	);
	if (iterator == pending_collision_retirements_.end() || *iterator != key) {
		pending_collision_retirements_.insert(iterator, key);
	}
}

void WorldTransvoxelTerrain::cancel_collision_retirement(
	const WtChunkKey &key
) {
	const auto iterator = std::lower_bound(
		pending_collision_retirements_.begin(),
		pending_collision_retirements_.end(),
		key
	);
	if (iterator != pending_collision_retirements_.end() && *iterator == key) {
		pending_collision_retirements_.erase(iterator);
	}
}

void WorldTransvoxelTerrain::flush_ready_collision_retirements() {
	if (open_viewer_plan_publications_ != 0 ||
			pending_collision_retirements_.empty()) {
		return;
	}
	std::vector<WtChunkKey> required;
	std::vector<WtChunkKey> physically_ready;
	const std::vector<WtChunkApplicationRecord> records =
		application_->get_records();
	required.reserve(records.size());
	physically_ready.reserve(records.size());
	for (const WtChunkApplicationRecord &record : records) {
		if (!record.collision_required) continue;
		required.push_back(record.key);
		if (!record.collision_ready) continue;
		const WtGenerationToken applied =
			collision_sink_->applied_generation(record.key);
		const WtGenerationToken staged =
			collision_sink_->staged_generation(record.key);
		if (applied.value != 0 || staged.value == 0) {
			physically_ready.push_back(record.key);
		}
	}
	constexpr std::size_t kMaximumRetirementsPerFrame = 32;
	std::size_t processed = 0;
	for (std::size_t index = 0;
			index < pending_collision_retirements_.size() &&
			processed < kMaximumRetirementsPerFrame;) {
		const WtChunkKey key = pending_collision_retirements_[index];
		if (!wt_collision_retirement_is_safe(key, required, physically_ready)) {
			++index;
			continue;
		}
		collision_sink_->remove_collision(key);
		pending_collision_retirements_.erase(
			pending_collision_retirements_.begin() + index
		);
		++processed;
	}
}

void WorldTransvoxelTerrain::clear_visibility_coverage_priority_request(
	const WtChunkKey &key
) {
	visibility_coverage_priority_requests_.erase(
		std::remove_if(
			visibility_coverage_priority_requests_.begin(),
			visibility_coverage_priority_requests_.end(),
			[&key](const CoveragePriorityRequest &request) {
				return request.key == key;
			}
		),
		visibility_coverage_priority_requests_.end()
	);
}

void WorldTransvoxelTerrain::request_visibility_coverage_priority_batch(
	const std::vector<WtChunkApplicationRecord> &records,
	std::size_t replacement_count,
	std::size_t retirement_count
) {
	std::vector<WtVisibilityCoveragePriorityRequest> requests;
	requests.reserve(records.size());
	for (const WtChunkApplicationRecord &record : records) {
		const auto existing = std::find_if(
			visibility_coverage_priority_requests_.begin(),
			visibility_coverage_priority_requests_.end(),
			[&record](const CoveragePriorityRequest &request) {
				return request.key == record.key &&
					request.generation == record.generation;
			}
		);
		if (existing != visibility_coverage_priority_requests_.end()) continue;
		clear_visibility_coverage_priority_request(record.key);
		requests.push_back({ record.key, record.generation });
	}
	if (requests.empty() || !lifecycle_ ||
		lifecycle_->request_visibility_coverage_priority_batch(requests) !=
			WtReadOnlyRuntimeStatus::Ok) {
		return;
	}
	for (const WtVisibilityCoveragePriorityRequest &request : requests) {
		visibility_coverage_priority_requests_.push_back({
			request.key,
			request.generation,
		});
		if (cpu_causal_trace_active_) {
			lifecycle_->record_frontend_visibility(
				WtCausalTraceEventKind::VisibilityCoveragePriorityRequested,
				&request.key,
				request.generation,
				replacement_count,
				retirement_count,
				0
			);
		}
	}
}

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
			) || !publication_region_has_complete_authoritative_coverage(region)) {
			++index;
			continue;
		}
		bool ready = true;
		std::vector<WtChunkApplicationRecord> missing_records;
		missing_records.reserve(region.replacements.size());
		std::vector<WtChunkApplicationRecord> replacement_records;
		replacement_records.reserve(region.replacements.size());
		for (const WtChunkKey &replacement : region.replacements) {
			WtChunkApplicationRecord record;
			if (!application_->copy_record(replacement, record)) {
				ready = false;
				break;
			}
			replacement_records.push_back(record);
			if (!record.fully_ready()) {
				missing_records.push_back(record);
				ready = false;
				continue;
			}
			clear_visibility_coverage_priority_request(replacement);
			if (
					!render_sink_->can_publish_staged_record(
						replacement,
						record.generation
					) ||
					!collision_sink_->can_publish_staged_record(
						replacement,
						record.generation
					)) {
				ready = false;
			}
		}
		if (!ready) {
			request_visibility_coverage_priority_batch(
				missing_records,
				region.replacements.size(),
				region.retirements.size()
			);
			return;
		}
		for (const WtChunkKey &retirement : region.retirements) {
			application_->forget_chunk(retirement);
			render_sink_->begin_render_retirement(retirement);
			collision_sink_->remove_collision(retirement);
			render_sink_->publish_staged_record(retirement);
		}
		bool published = true;
		for (const WtChunkKey &replacement : region.replacements) {
			clear_visibility_coverage_priority_request(replacement);
			published = render_sink_->publish_staged_record(replacement) &&
				collision_sink_->publish_staged_record(replacement) && published;
		}
		if (!published) {
			synchronous_world_error_ =
				"regional visibility publication failed";
			return;
		}
		const std::uint64_t publication_cohort =
			regional_visibility_publications_ + 1U;
		if (cpu_causal_trace_active_ && lifecycle_) {
			lifecycle_->record_frontend_visibility(
				WtCausalTraceEventKind::VisibilityRegionDesiredSnapshot,
				nullptr,
				{ publication_cohort },
				latest_completed_viewer_plan_revision_,
				open_viewer_plan_publications_,
				0
			);
			for (std::size_t member = 0;
					member < region.replacements.size(); ++member) {
				const WtChunkApplicationRecord &record =
					replacement_records[member];
				const std::int64_t desired_roles =
					(record.visual_required ? 1 : 0) |
					(record.collision_required ? 2 : 0) |
					(record.staged_replacement ? 4 : 0) |
					(record.fully_ready() ? 8 : 0);
				lifecycle_->record_frontend_visibility(
					WtCausalTraceEventKind::VisibilityRegionReplacementMember,
					&region.replacements[member],
					record.generation,
					publication_cohort,
					0,
					desired_roles
				);
			}
			for (const WtChunkKey &retirement : region.retirements) {
				lifecycle_->record_frontend_visibility(
					WtCausalTraceEventKind::VisibilityRegionRetirementMember,
					&retirement,
					{},
					publication_cohort,
					region.replacements.size(),
					static_cast<std::int64_t>(region.retirements.size())
				);
			}
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
		if (cpu_causal_trace_active_ && lifecycle_) {
			lifecycle_->record_frontend_visibility(
				WtCausalTraceEventKind::VisibilityBatchPublished,
				nullptr,
				{ publication_cohort },
				region.replacements.size(),
				region.retirements.size(),
				1
			);
		}
		index = 0;
	}
}

} // namespace world_transvoxel
