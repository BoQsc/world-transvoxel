#include "api/world_transvoxel_terrain.h"

#include "services/wt_chunk_application.h"

#include <limits>

namespace world_transvoxel {

bool WorldTransvoxelTerrain::begin_cpu_causal_trace() {
	if (!lifecycle_ || !lifecycle_->begin_causal_trace()) return false;
	application_->set_trace_observer(
		[this](
			bool collision,
			const WtChunkKey &key,
			WtGenerationToken generation,
			std::uint64_t duration_ns,
			bool applied
		) {
			if (lifecycle_) lifecycle_->record_frontend_sink(
				collision,
				key,
				generation,
				duration_ns,
				applied
			);
		}
	);
	cpu_causal_trace_active_ = true;
	trace_pending_replacements_ = std::numeric_limits<std::size_t>::max();
	trace_pending_retirements_ = std::numeric_limits<std::size_t>::max();
	trace_pending_render_retirements_ = std::numeric_limits<std::size_t>::max();
	return true;
}

void WorldTransvoxelTerrain::end_cpu_causal_trace() {
	cpu_causal_trace_active_ = false;
	application_->set_trace_observer({});
	if (lifecycle_) lifecycle_->end_causal_trace();
}

godot::Dictionary WorldTransvoxelTerrain::get_cpu_causal_trace_events(
	std::int64_t first_sequence,
	std::int64_t maximum_events
) const {
	godot::Dictionary output;
	if (first_sequence < 0 || maximum_events <= 0 ||
		maximum_events > 65536) {
		output["valid"] = false;
		return output;
	}
	const WtCausalTraceSnapshot snapshot = lifecycle_ ?
		lifecycle_->causal_trace_snapshot(
			static_cast<std::uint64_t>(first_sequence),
			static_cast<std::size_t>(maximum_events)
		) : WtCausalTraceSnapshot{};
	output["valid"] = true;
	output["enabled"] = snapshot.enabled;
	output["capacity"] = static_cast<std::int64_t>(snapshot.capacity);
	output["retained_event_count"] = static_cast<std::int64_t>(
		snapshot.retained_event_count
	);
	output["dropped_event_count"] = static_cast<std::int64_t>(
		snapshot.dropped_event_count
	);
	output["first_retained_sequence"] = static_cast<std::int64_t>(
		snapshot.first_retained_sequence
	);
	output["next_sequence"] = static_cast<std::int64_t>(
		snapshot.next_sequence
	);
	godot::Array events;
	events.resize(static_cast<int>(snapshot.events.size()));
	for (std::size_t index = 0; index < snapshot.events.size(); ++index) {
		const WtCausalTraceEvent &event = snapshot.events[index];
		godot::Dictionary encoded;
		encoded["sequence"] = static_cast<std::int64_t>(event.sequence);
		encoded["elapsed_ns"] = static_cast<std::int64_t>(event.elapsed_ns);
		encoded["duration_ns"] = static_cast<std::int64_t>(event.duration_ns);
		encoded["kind"] = wt_causal_trace_event_kind_name(event.kind);
		encoded["thread"] = wt_causal_trace_thread_role_name(event.thread_role);
		encoded["has_chunk"] = event.has_chunk;
		if (event.has_chunk) {
			encoded["chunk_x"] = event.key.x;
			encoded["chunk_y"] = event.key.y;
			encoded["chunk_z"] = event.key.z;
			encoded["chunk_lod"] = event.key.lod;
		}
		encoded["generation"] = static_cast<std::int64_t>(
			event.generation.value
		);
		encoded["cause_id"] = static_cast<std::int64_t>(event.cause_id);
		encoded["auxiliary"] = static_cast<std::int64_t>(event.auxiliary);
		encoded["status"] = event.status;
		encoded["has_job_details"] = event.has_job_details;
		if (event.has_job_details) {
			encoded["job_stage"] =
				event.job.stage == WtCausalTraceJobStage::Sample ?
					"sample" : "mesh";
			encoded["effective_priority"] = event.job.effective_priority;
			encoded["job_sequence"] = static_cast<std::int64_t>(
				event.job.sequence
			);
			encoded["has_queue_state"] = event.job.has_queue_state;
			if (event.job.has_queue_state) {
				encoded["queue_depth_before"] = static_cast<std::int64_t>(
					event.job.queue_depth_before
				);
				encoded["queue_depth_after"] = static_cast<std::int64_t>(
					event.job.queue_depth_after
				);
				encoded["jobs_ahead"] = static_cast<std::int64_t>(
					event.job.jobs_ahead
				);
				encoded["same_priority_jobs_ahead"] =
					static_cast<std::int64_t>(
						event.job.same_priority_jobs_ahead
					);
			}
		}
		events[static_cast<int>(index)] = encoded;
	}
	output["events"] = events;
	return output;
}

} // namespace world_transvoxel
