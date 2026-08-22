#include "api/world_transvoxel_terrain.h"

#include "physics/wt_godot_collision_sink.h"
#include "render/wt_godot_render_sink.h"
#include "services/wt_chunk_application.h"

#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>

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
	godot::ClassDB::bind_method(
		godot::D_METHOD("begin_cpu_causal_trace"),
		&WorldTransvoxelTerrain::begin_cpu_causal_trace
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("end_cpu_causal_trace"),
		&WorldTransvoxelTerrain::end_cpu_causal_trace
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"get_cpu_causal_trace_events",
			"first_sequence",
			"maximum_events"
		),
		&WorldTransvoxelTerrain::get_cpu_causal_trace_events
	);
}

godot::Dictionary WorldTransvoxelTerrain::get_runtime_metrics() const {
	const WtReadOnlyRuntimeMetrics runtime = lifecycle_ ?
		lifecycle_->runtime_metrics() : WtReadOnlyRuntimeMetrics{};
	const WtApplicationMetrics application = application_->get_metrics();
	std::uint64_t visual_ready_records = 0;
	std::uint64_t visual_required_records = 0;
	std::uint64_t collision_ready_records = 0;
	std::uint64_t collision_required_records = 0;
	std::uint64_t collision_required_not_ready_records = 0;
	WtChunkKey first_collision_not_ready_key{};
	std::uint64_t first_collision_not_ready_generation = 0;
	bool first_collision_not_ready_visual_required = false;
	bool first_collision_not_ready_visual_ready = false;
	bool first_collision_not_ready_staged = false;
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
	bool first_blocked_replacement_render_record_present = false;
	bool first_blocked_replacement_render_staged = false;
	std::uint64_t first_blocked_replacement_generation = 0;
	std::uint64_t first_blocked_replacement_render_generation = 0;
	std::uint64_t first_blocked_replacement_staged_render_generation = 0;
	for (const WtChunkApplicationRecord &record : application_->get_records()) {
		visual_ready_records += record.visual_ready ? 1U : 0U;
		visual_required_records += record.visual_required ? 1U : 0U;
		collision_ready_records += record.collision_ready ? 1U : 0U;
		collision_required_records += record.collision_required ? 1U : 0U;
		if (record.collision_required && !record.collision_ready) {
			if (collision_required_not_ready_records == 0) {
				first_collision_not_ready_key = record.key;
				first_collision_not_ready_generation = record.generation.value;
				first_collision_not_ready_visual_required =
					record.visual_required;
				first_collision_not_ready_visual_ready = record.visual_ready;
				first_collision_not_ready_staged = record.staged_replacement;
			}
			++collision_required_not_ready_records;
		}
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
		WtChunkApplicationRecord record;
		const bool record_present = application_->copy_record(key, record);
		if (record_present && record.fully_ready()) continue;
		if (blocked_pending_replacements == 0) {
			first_blocked_replacement_key = key;
			first_blocked_replacement_missing = !record_present;
			if (record_present) {
				first_blocked_replacement_visual_required =
					record.visual_required;
				first_blocked_replacement_visual_ready = record.visual_ready;
				first_blocked_replacement_collision_required =
					record.collision_required;
				first_blocked_replacement_collision_ready =
					record.collision_ready;
				first_blocked_replacement_staged = record.staged_replacement;
				first_blocked_replacement_generation =
					record.generation.value;
				first_blocked_replacement_render_generation =
					render_sink_->applied_generation(key).value;
				first_blocked_replacement_render_record_present =
					render_sink_->has_record(key);
				first_blocked_replacement_render_staged =
					render_sink_->has_staged_record(key);
				first_blocked_replacement_staged_render_generation =
					render_sink_->staged_generation(key).value;
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
	set_metric(
		output, "sample_job_time_ns_last", runtime.sample_job_time_ns_last
	);
	set_metric(
		output, "sample_job_time_ns_total", runtime.sample_job_time_ns_total
	);
	set_metric(
		output,
		"sample_job_time_ns_maximum",
		runtime.sample_job_time_ns_maximum
	);
	set_metric(output, "mesh_job_time_ns_last", runtime.mesh_job_time_ns_last);
	set_metric(output, "mesh_job_time_ns_total", runtime.mesh_job_time_ns_total);
	set_metric(
		output, "mesh_job_time_ns_maximum", runtime.mesh_job_time_ns_maximum
	);
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
		output, "edit_transaction_attempts", runtime.edit_transaction_attempts
	);
	set_metric(
		output,
		"edit_completed_transactions",
		runtime.edit_completed_transactions
	);
	set_metric(output, "edit_empty_transactions", runtime.edit_empty_transactions);
	set_metric(output, "edit_queried_chunks", runtime.edit_queried_chunks);
	set_metric(output, "edit_replaced_chunks", runtime.edit_replaced_chunks);
	set_metric(
		output, "edit_evicted_page_entries", runtime.edit_evicted_page_entries
	);
	set_metric(
		output,
		"edit_evicted_resource_entries",
		runtime.edit_evicted_resource_entries
	);
	set_metric(output, "edit_spatial_rejections", runtime.edit_spatial_rejections);
	set_metric(output, "edit_capacity_rejections", runtime.edit_capacity_rejections);
	set_metric(output, "edit_state_rejections", runtime.edit_state_rejections);
	set_metric(output, "edit_scheduler_failures", runtime.edit_scheduler_failures);
	set_metric(output, "edit_application_failures", runtime.edit_application_failures);
	set_metric(
		output,
		"edit_page_meshing_runtime_failures",
		runtime.edit_page_meshing_runtime_failures
	);
	set_metric(
		output,
		"edit_cancelled_page_meshing_generations",
		runtime.edit_cancelled_page_meshing_generations
	);
	set_metric(
		output,
		"visibility_coverage_priority_requests",
		runtime.visibility_coverage_priority_requests
	);
	set_metric(
		output,
		"visibility_coverage_priority_applied",
		runtime.visibility_coverage_priority_applied
	);
	set_metric(
		output,
		"visibility_coverage_priority_stale",
		runtime.visibility_coverage_priority_stale
	);
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
	set_metric(output, "storage_queued_requests", runtime.storage_queued_requests);
	set_metric(
		output,
		"storage_queued_completions",
		runtime.storage_queued_completions
	);
	set_metric(output, "storage_active_requests", runtime.storage_active_requests);
	set_metric(
		output,
		"storage_accepted_requests",
		runtime.storage_accepted_requests
	);
	set_metric(
		output,
		"storage_started_requests",
		runtime.storage_started_requests
	);
	set_metric(
		output,
		"storage_completed_requests",
		runtime.storage_completed_requests
	);
	set_metric(
		output,
		"storage_request_queue_rejections",
		runtime.storage_request_queue_rejections
	);
	set_metric(
		output,
		"storage_duplicate_requests",
		runtime.storage_duplicate_requests
	);
	set_metric(
		output,
		"storage_successful_pages",
		runtime.storage_successful_pages
	);
	set_metric(
		output,
		"storage_load_time_ns_last",
		runtime.storage_load_time_ns_last
	);
	set_metric(
		output,
		"storage_load_time_ns_total",
		runtime.storage_load_time_ns_total
	);
	set_metric(
		output,
		"storage_load_time_ns_maximum",
		runtime.storage_load_time_ns_maximum
	);
	set_metric(output, "storage_worker_count", runtime.storage_worker_count);
	set_metric(
		output,
		"storage_in_flight_requests",
		runtime.storage_in_flight_requests
	);
	set_metric(
		output,
		"storage_maximum_in_flight_requests",
		runtime.storage_maximum_in_flight_requests
	);
	set_metric(
		output,
		"storage_in_flight_elapsed_ns",
		runtime.storage_in_flight_elapsed_ns
	);
	set_metric(
		output,
		"storage_in_flight_key_x",
		runtime.storage_in_flight_key_x
	);
	set_metric(
		output,
		"storage_in_flight_key_y",
		runtime.storage_in_flight_key_y
	);
	set_metric(
		output,
		"storage_in_flight_key_z",
		runtime.storage_in_flight_key_z
	);
	set_metric(
		output,
		"storage_in_flight_key_lod",
		runtime.storage_in_flight_key_lod
	);
	set_metric(
		output,
		"storage_in_flight_generation",
		runtime.storage_in_flight_generation
	);
	set_metric(output, "hierarchy_kind", runtime.hierarchy_kind);
	set_metric(
		output,
		"hierarchy_declared_pages",
		runtime.hierarchy_declared_pages
	);
	set_metric(
		output,
		"hierarchy_explicit_index_entries",
		runtime.hierarchy_explicit_index_entries
	);
	set_metric(
		output,
		"hierarchy_estimated_index_bytes",
		runtime.hierarchy_estimated_index_bytes
	);
	set_metric(
		output,
		"hierarchy_sparse_overlay_entries",
		runtime.hierarchy_sparse_overlay_entries
	);
	set_metric(
		output,
		"hierarchy_sparse_overlay_index_bytes",
		runtime.hierarchy_sparse_overlay_index_bytes
	);
	set_metric(
		output,
		"hierarchy_membership_queries",
		runtime.hierarchy_membership_queries
	);
	set_metric(
		output,
		"hierarchy_child_queries",
		runtime.hierarchy_child_queries
	);
	set_metric(
		output,
		"hierarchy_ancestor_queries",
		runtime.hierarchy_ancestor_queries
	);
	set_metric(
		output,
		"hierarchy_neighbor_queries",
		runtime.hierarchy_neighbor_queries
	);
	set_metric(
		output,
		"hierarchy_range_queries",
		runtime.hierarchy_range_queries
	);
	set_metric(
		output,
		"hierarchy_viewer_root_queries",
		runtime.hierarchy_viewer_root_queries
	);
	set_metric(
		output,
		"hierarchy_lod_enumerations",
		runtime.hierarchy_lod_enumerations
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
	set_metric(output, "page_dependency_requests", runtime.page_dependency_requests);
	set_metric(
		output,
		"page_dependency_reprioritizations",
		runtime.page_dependency_reprioritizations
	);
	set_metric(
		output,
		"page_dependency_cache_hits",
		runtime.page_dependency_cache_hits
	);
	set_metric(
		output,
		"page_dependency_cache_misses",
		runtime.page_dependency_cache_misses
	);
	set_metric(
		output,
		"page_accepted_storage_completions",
		runtime.page_accepted_storage_completions
	);
	set_metric(
		output,
		"page_stale_storage_completions",
		runtime.page_stale_storage_completions
	);
	set_metric(
		output,
		"page_cache_encoded_entries",
		runtime.page_cache_encoded_entries
	);
	set_metric(
		output,
		"page_cache_decoded_entries",
		runtime.page_cache_decoded_entries
	);
	set_metric(
		output,
		"page_cache_encoded_hits",
		runtime.page_cache_encoded_hits
	);
	set_metric(
		output,
		"page_cache_encoded_misses",
		runtime.page_cache_encoded_misses
	);
	set_metric(
		output,
		"page_cache_encoded_insertions",
		runtime.page_cache_encoded_insertions
	);
	set_metric(
		output,
		"page_cache_encoded_refreshes",
		runtime.page_cache_encoded_refreshes
	);
	set_metric(
		output,
		"page_cache_encoded_evictions",
		runtime.page_cache_encoded_evictions
	);
	set_metric(
		output,
		"page_cache_decoded_hits",
		runtime.page_cache_decoded_hits
	);
	set_metric(
		output,
		"page_cache_decoded_misses",
		runtime.page_cache_decoded_misses
	);
	set_metric(
		output,
		"page_cache_decoded_insertions",
		runtime.page_cache_decoded_insertions
	);
	set_metric(
		output,
		"page_cache_decoded_evictions",
		runtime.page_cache_decoded_evictions
	);
	set_metric(output, "page_loading_records", runtime.page_loading_records);
	set_metric(
		output,
		"page_sample_ready_records",
		runtime.page_sample_ready_records
	);
	set_metric(
		output,
		"page_awaiting_mesh_records",
		runtime.page_awaiting_mesh_records
	);
	set_metric(output, "page_mesh_ready_records", runtime.page_mesh_ready_records);
	set_metric(output, "page_ready_records", runtime.page_ready_records);
	set_metric(
		output,
		"page_unresolved_dependencies",
		runtime.page_unresolved_dependencies
	);
	set_metric(
		output,
		"page_pending_dependency_requests",
		runtime.page_pending_dependency_requests
	);
	set_metric(output, "page_pinned_pages", runtime.page_pinned_pages);
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
	set_metric(
		output,
		"collision_apply_time_ns_last",
		application.collision_apply_time_ns_last
	);
	set_metric(
		output,
		"collision_apply_time_ns_maximum",
		application.collision_apply_time_ns_maximum
	);
	set_metric(
		output,
		"collision_apply_time_ns_total",
		application.collision_apply_time_ns_total
	);
	set_metric(
		output,
		"collision_apply_deadline_exhaustions",
		application.collision_apply_deadline_exhaustions
	);
	set_metric(
		output,
		"collision_apply_deadline_ns",
		collision_apply_deadline_ns_
	);
	set_metric(
		output,
		"collision_apply_frame_time_ns_last",
		collision_apply_frame_time_ns_last_
	);
	set_metric(
		output,
		"collision_apply_frame_time_ns_total",
		collision_apply_frame_time_ns_total_
	);
	set_metric(
		output,
		"collision_apply_frame_time_ns_maximum",
		collision_apply_frame_time_ns_maximum_
	);
	set_metric(
		output,
		"collision_apply_frame_items_last",
		collision_apply_frame_items_last_
	);
	set_metric(
		output,
		"collision_apply_frame_items_maximum",
		collision_apply_frame_items_maximum_
	);
	set_metric(
		output,
		"collision_apply_frame_deadline_overruns",
		collision_apply_frame_deadline_overruns_
	);
	output["active_chunk_records"] = static_cast<std::int64_t>(
		application_->get_records().size()
	);
	set_metric(output, "visual_ready_chunk_records", visual_ready_records);
	set_metric(output, "visual_required_chunk_records", visual_required_records);
	set_metric(output, "collision_ready_chunk_records", collision_ready_records);
	set_metric(
		output,
		"collision_required_chunk_records",
		collision_required_records
	);
	set_metric(
		output,
		"collision_required_not_ready_chunk_records",
		collision_required_not_ready_records
	);
	output["first_collision_not_ready_key_x"] =
		static_cast<std::int64_t>(first_collision_not_ready_key.x);
	output["first_collision_not_ready_key_y"] =
		static_cast<std::int64_t>(first_collision_not_ready_key.y);
	output["first_collision_not_ready_key_z"] =
		static_cast<std::int64_t>(first_collision_not_ready_key.z);
	output["first_collision_not_ready_key_lod"] =
		static_cast<std::int64_t>(first_collision_not_ready_key.lod);
	set_metric(
		output,
		"first_collision_not_ready_generation",
		first_collision_not_ready_generation
	);
	output["first_collision_not_ready_visual_required"] =
		first_collision_not_ready_visual_required;
	output["first_collision_not_ready_visual_ready"] =
		first_collision_not_ready_visual_ready;
	output["first_collision_not_ready_staged"] =
		first_collision_not_ready_staged;
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
	output["ready_staged_chunk_replacements"] = static_cast<std::int64_t>(
		ready_staged_chunk_replacements_.size()
	);
	set_metric(
		output,
		"open_viewer_plan_publications",
		open_viewer_plan_publications_
	);
	set_metric(
		output,
		"latest_completed_viewer_plan_revision",
		latest_completed_viewer_plan_revision_
	);
	set_metric(
		output,
		"regional_visibility_publications",
		regional_visibility_publications_
	);
	set_metric(
		output,
		"regional_visibility_replacements",
		regional_visibility_replacements_
	);
	set_metric(
		output,
		"regional_visibility_retirements",
		regional_visibility_retirements_
	);
	output["relocation_visibility_prewarm_pending"] =
		relocation_visibility_prewarm_pending_;
	set_metric(
		output,
		"relocation_visibility_prewarm_requests",
		relocation_visibility_prewarm_requests_
	);
	set_metric(
		output,
		"relocation_visibility_prewarm_coalesced",
		relocation_visibility_prewarm_coalesced_
	);
	set_metric(
		output,
		"relocation_visibility_prewarm_attempts",
		relocation_visibility_prewarm_attempts_
	);
	set_metric(
		output,
		"relocation_visibility_prewarm_batches",
		relocation_visibility_prewarm_batches_
	);
	set_metric(
		output,
		"relocation_visibility_prewarm_records",
		relocation_visibility_prewarm_records_
	);
	output["visibility_coverage_priority_pending"] =
		static_cast<std::int64_t>(
			visibility_coverage_priority_requests_.size()
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
	output["first_blocked_replacement_render_record_present"] =
		first_blocked_replacement_render_record_present;
	output["first_blocked_replacement_render_staged"] =
		first_blocked_replacement_render_staged;
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
	set_metric(
		output,
		"first_blocked_replacement_staged_render_generation",
		first_blocked_replacement_staged_render_generation
	);
	output["pending_render_retirements"] = static_cast<std::int64_t>(
		pending_render_retirements_.size()
	);
	output["queued_render"] = get_queued_render_count();
	output["queued_collision"] = get_queued_collision_count();
	output["deferred_collision"] = static_cast<std::int64_t>(
		application_->deferred_collision_count()
	);
	output["total_collision_backlog"] = static_cast<std::int64_t>(
		application_->queued_collision_count() +
		application_->deferred_collision_count()
	);
	output["render_resources"] = get_render_resource_count();
	output["render_fading_resources"] = static_cast<std::int64_t>(
		render_sink_->fading_count()
	);
	output["staged_render_resources"] = static_cast<std::int64_t>(
		render_sink_->staged_count()
	);
	output["collision_resources"] = get_collision_resource_count();
	output["staged_collision_resources"] = static_cast<std::int64_t>(
		collision_sink_->staged_count()
	);
	return output;
}

} // namespace world_transvoxel
