#include "api/world_transvoxel_terrain.h"

#include "render/wt_godot_render_sink.h"
#include "services/wt_chunk_application.h"

#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cstdint>

namespace world_transvoxel {
namespace {

void set_metric(
	godot::Dictionary &output,
	const char *name,
	std::uint64_t value
) {
	output[name] = static_cast<std::int64_t>(value);
}

} // namespace

void WorldTransvoxelTerrain::bind_metrics_methods() {
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_runtime_metrics"),
		&WorldTransvoxelTerrain::get_runtime_metrics
	);
}

godot::Dictionary WorldTransvoxelTerrain::get_runtime_metrics() const {
	const WtReadOnlyRuntimeMetrics runtime = lifecycle_ ?
		lifecycle_->runtime_metrics() : WtReadOnlyRuntimeMetrics{};
	const WtApplicationMetrics application = application_->get_metrics();
	std::uint64_t visual_ready_records = 0;
	std::uint64_t visual_required_records = 0;
	std::uint64_t fully_ready_records = 0;
	std::uint64_t non_retiring_records = 0;
	std::uint64_t non_retiring_visual_ready_records = 0;
	std::uint64_t non_retiring_fully_ready_records = 0;
	std::uint64_t pending_retirement_records = 0;
	std::uint64_t blocked_pending_replacements = 0;
	WtChunkKey first_blocked_replacement_key{};
	bool first_blocked_replacement_missing = false;
	bool first_blocked_replacement_visual_required = false;
	bool first_blocked_replacement_visual_ready = false;
	bool first_blocked_replacement_collision_required = false;
	bool first_blocked_replacement_collision_ready = false;
	bool first_blocked_replacement_staged = false;
	std::uint64_t first_blocked_replacement_generation = 0;
	std::uint64_t first_blocked_replacement_render_generation = 0;
	for (const WtChunkApplicationRecord &record : application_->get_records()) {
		visual_ready_records += record.visual_ready ? 1U : 0U;
		visual_required_records += record.visual_required ? 1U : 0U;
		fully_ready_records += record.fully_ready() ? 1U : 0U;
		const bool pending_retirement = std::binary_search(
			pending_chunk_retirements_.begin(),
			pending_chunk_retirements_.end(),
			record.key
		);
		if (pending_retirement) {
			++pending_retirement_records;
		} else {
			++non_retiring_records;
			non_retiring_visual_ready_records +=
				(!record.visual_required || record.visual_ready) ? 1U : 0U;
			non_retiring_fully_ready_records += record.fully_ready() ? 1U : 0U;
		}
	}
	for (const WtChunkKey &key : pending_chunk_replacements_) {
		const WtChunkApplicationRecord *record = application_->find_record(key);
		if (record != nullptr && record->fully_ready()) continue;
		if (blocked_pending_replacements == 0) {
			first_blocked_replacement_key = key;
			first_blocked_replacement_missing = record == nullptr;
			if (record != nullptr) {
				first_blocked_replacement_visual_required =
					record->visual_required;
				first_blocked_replacement_visual_ready = record->visual_ready;
				first_blocked_replacement_collision_required =
					record->collision_required;
				first_blocked_replacement_collision_ready =
					record->collision_ready;
				first_blocked_replacement_staged = record->staged_replacement;
				first_blocked_replacement_generation =
					record->generation.value;
				first_blocked_replacement_render_generation =
					render_sink_->applied_generation(key).value;
			}
		}
		++blocked_pending_replacements;
	}
	const std::uint64_t pending_retirement_records_missing =
		pending_chunk_retirements_.size() > pending_retirement_records ?
		static_cast<std::uint64_t>(
			pending_chunk_retirements_.size() - pending_retirement_records
		) : 0U;
	godot::Dictionary output;
	output["world_running"] = is_world_running();
	set_metric(output, "viewer_updates", runtime.viewer_updates);
	set_metric(output, "viewer_removals", runtime.viewer_removals);
	set_metric(
		output,
		"collision_viewer_updates",
		runtime.collision_viewer_updates
	);
	set_metric(
		output,
		"collision_viewer_removals",
		runtime.collision_viewer_removals
	);
	set_metric(
		output, "coalesced_viewer_events", runtime.coalesced_viewer_events
	);
	set_metric(output, "planned_demands", runtime.planned_demands);
	set_metric(output, "sample_jobs", runtime.sample_jobs);
	set_metric(output, "mesh_jobs", runtime.mesh_jobs);
	set_metric(output, "storage_completions", runtime.storage_completions);
	set_metric(output, "mesh_completions", runtime.mesh_completions);
	set_metric(
		output,
		"transition_mesh_completions",
		runtime.transition_mesh_completions
	);
	set_metric(output, "edit_commits", runtime.edit_commits);
	set_metric(output, "edit_rejections", runtime.edit_rejections);
	set_metric(output, "edit_replacements", runtime.edit_replacements);
	set_metric(
		output,
		"edit_lod_retention_zones",
		runtime.edit_lod_retention_zones
	);
	set_metric(
		output,
		"edit_lod_retention_active_viewers",
		runtime.edit_lod_retention_active_viewers
	);
	set_metric(
		output,
		"edit_lod_retention_plans",
		runtime.edit_lod_retention_plans
	);
	set_metric(
		output,
		"edit_lod_retention_fallbacks",
		runtime.edit_lod_retention_fallbacks
	);
	set_metric(output, "sample_queries", runtime.sample_queries);
	set_metric(
		output, "sample_query_rejections", runtime.sample_query_rejections
	);
	set_metric(output, "world_snapshots", runtime.world_snapshots);
	set_metric(
		output,
		"world_snapshot_rejections",
		runtime.world_snapshot_rejections
	);
	set_metric(output, "published_events", runtime.published_events);
	set_metric(output, "rejected_events", runtime.rejected_events);
	set_metric(
		output,
		"scheduler_requested_records",
		runtime.scheduler_requested_records
	);
	set_metric(
		output,
		"scheduler_sampling_records",
		runtime.scheduler_sampling_records
	);
	set_metric(
		output,
		"scheduler_meshing_records",
		runtime.scheduler_meshing_records
	);
	set_metric(
		output,
		"scheduler_ready_records",
		runtime.scheduler_ready_records
	);
	set_metric(
		output,
		"scheduler_failed_records",
		runtime.scheduler_failed_records
	);
	set_metric(output, "scheduler_queued_jobs", runtime.scheduler_queued_jobs);
	set_metric(
		output,
		"scheduler_queued_completions",
		runtime.scheduler_queued_completions
	);
	set_metric(
		output,
		"scheduler_queue_rejections",
		runtime.scheduler_queue_rejections
	);
	set_metric(output, "page_sample_failures", runtime.page_sample_failures);
	set_metric(output, "page_mesh_failures", runtime.page_mesh_failures);
	set_metric(output, "page_storage_failures", runtime.page_storage_failures);
	set_metric(output, "page_cache_failures", runtime.page_cache_failures);
	set_metric(
		output,
		"page_scheduler_backpressure",
		runtime.page_scheduler_backpressure
	);
	output["page_last_failure_key_x"] =
		static_cast<std::int64_t>(runtime.page_last_failure_key_x);
	output["page_last_failure_key_y"] =
		static_cast<std::int64_t>(runtime.page_last_failure_key_y);
	output["page_last_failure_key_z"] =
		static_cast<std::int64_t>(runtime.page_last_failure_key_z);
	output["page_last_failure_key_lod"] =
		static_cast<std::int64_t>(runtime.page_last_failure_key_lod);
	set_metric(
		output, "application_submitted_render", application.submitted_render
	);
	set_metric(
		output,
		"application_submitted_collision",
		application.submitted_collision
	);
	set_metric(output, "application_applied_render", application.applied_render);
	set_metric(
		output,
		"application_applied_collision",
		application.applied_collision
	);
	set_metric(output, "application_stale_render", application.stale_render);
	output["application_last_stale_render_key_x"] =
		application.last_stale_render_key_x;
	output["application_last_stale_render_key_y"] =
		application.last_stale_render_key_y;
	output["application_last_stale_render_key_z"] =
		application.last_stale_render_key_z;
	output["application_last_stale_render_key_lod"] =
		static_cast<std::int64_t>(application.last_stale_render_key_lod);
	set_metric(
		output,
		"application_last_stale_render_generation",
		application.last_stale_render_generation
	);
	set_metric(
		output,
		"application_last_stale_render_record_generation",
		application.last_stale_render_record_generation
	);
	set_metric(
		output, "application_stale_collision", application.stale_collision
	);
	set_metric(
		output,
		"application_unrequired_collision",
		application.unrequired_collision
	);
	set_metric(
		output, "application_sink_failures", application.sink_failures
	);
	set_metric(
		output, "application_queue_rejections", application.queue_rejections
	);
	set_metric(
		output,
		"render_latency_frames_maximum",
		application.render_latency_frames_maximum
	);
	set_metric(
		output,
		"collision_latency_frames_maximum",
		application.collision_latency_frames_maximum
	);
	output["active_chunk_records"] = static_cast<std::int64_t>(
		application_->get_records().size()
	);
	set_metric(output, "visual_ready_chunk_records", visual_ready_records);
	set_metric(output, "visual_required_chunk_records", visual_required_records);
	set_metric(output, "fully_ready_chunk_records", fully_ready_records);
	set_metric(output, "non_retiring_chunk_records", non_retiring_records);
	set_metric(
		output,
		"non_retiring_visual_ready_chunk_records",
		non_retiring_visual_ready_records
	);
	set_metric(
		output,
		"non_retiring_fully_ready_chunk_records",
		non_retiring_fully_ready_records
	);
	set_metric(output, "pending_retirement_records", pending_retirement_records);
	set_metric(
		output,
		"pending_retirement_records_missing",
		pending_retirement_records_missing
	);
	output["pending_chunk_retirements"] = static_cast<std::int64_t>(
		pending_chunk_retirements_.size()
	);
	output["pending_chunk_replacements"] = static_cast<std::int64_t>(
		pending_chunk_replacements_.size()
	);
	set_metric(
		output,
		"blocked_pending_chunk_replacements",
		blocked_pending_replacements
	);
	output["first_blocked_replacement_key_x"] =
		static_cast<std::int64_t>(first_blocked_replacement_key.x);
	output["first_blocked_replacement_key_y"] =
		static_cast<std::int64_t>(first_blocked_replacement_key.y);
	output["first_blocked_replacement_key_z"] =
		static_cast<std::int64_t>(first_blocked_replacement_key.z);
	output["first_blocked_replacement_key_lod"] =
		static_cast<std::int64_t>(first_blocked_replacement_key.lod);
	output["first_blocked_replacement_missing"] =
		first_blocked_replacement_missing;
	output["first_blocked_replacement_visual_required"] =
		first_blocked_replacement_visual_required;
	output["first_blocked_replacement_visual_ready"] =
		first_blocked_replacement_visual_ready;
	output["first_blocked_replacement_collision_required"] =
		first_blocked_replacement_collision_required;
	output["first_blocked_replacement_collision_ready"] =
		first_blocked_replacement_collision_ready;
	output["first_blocked_replacement_staged"] =
		first_blocked_replacement_staged;
	set_metric(
		output,
		"first_blocked_replacement_generation",
		first_blocked_replacement_generation
	);
	set_metric(
		output,
		"first_blocked_replacement_render_generation",
		first_blocked_replacement_render_generation
	);
	output["pending_render_retirements"] = static_cast<std::int64_t>(
		pending_render_retirements_.size()
	);
	output["queued_render"] = get_queued_render_count();
	output["queued_collision"] = get_queued_collision_count();
	output["render_resources"] = get_render_resource_count();
	output["render_fading_resources"] = static_cast<std::int64_t>(
		render_sink_->fading_count()
	);
	output["staged_render_resources"] = static_cast<std::int64_t>(
		render_sink_->staged_count()
	);
	output["collision_resources"] = get_collision_resource_count();
	return output;
}

} // namespace world_transvoxel
