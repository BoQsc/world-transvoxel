#include "telemetry/wt_causal_trace.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>

namespace wt = world_transvoxel;

namespace {

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

} // namespace

int main() {
	wt::WtCausalTraceBuffer trace;
	const wt::WtChunkKey first_key{ 4, -2, 7, 1 };
	check(
		std::string_view(wt::wt_causal_trace_event_kind_name(
			wt::WtCausalTraceEventKind::VisibilityRegionDesiredSnapshot
		)) == "visibility_region_desired_snapshot",
		"desired ownership snapshot event name mismatch"
	);
	check(
		std::string_view(wt::wt_causal_trace_event_kind_name(
			wt::WtCausalTraceEventKind::TransitionRemeshGenerationCreated
		)) == "transition_remesh_generation_created" &&
		std::string_view(wt::wt_causal_trace_event_kind_name(
			wt::WtCausalTraceEventKind::ReadinessRepairGenerationCreated
		)) == "readiness_repair_generation_created" &&
		std::string_view(wt::wt_causal_trace_event_kind_name(
			wt::WtCausalTraceEventKind::VisibilityCoveragePriorityOutcome
		)) == "visibility_coverage_priority_outcome" &&
		std::string_view(wt::wt_causal_trace_event_kind_name(
			wt::WtCausalTraceEventKind::SchedulerJobQueued
		)) == "scheduler_job_queued" &&
		std::string_view(wt::wt_causal_trace_event_kind_name(
			wt::WtCausalTraceEventKind::SchedulerJobPriorityObserved
		)) == "scheduler_job_priority_observed" &&
		std::string_view(wt::wt_causal_trace_event_kind_name(
			wt::WtCausalTraceEventKind::SchedulerJobDequeued
		)) == "scheduler_job_dequeued" &&
		std::string_view(wt::wt_causal_trace_event_kind_name(
			wt::WtCausalTraceEventKind::PageMeshingOwnershipEstablished
		)) == "page_meshing_ownership_established",
		"generation-origin, priority-outcome, or queue event name mismatch"
	);
	check(!trace.enabled(), "trace must be disabled by default");
	trace.record(
		wt::WtCausalTraceEventKind::StorageStarted,
		wt::WtCausalTraceThreadRole::Storage,
		&first_key,
		{ 8 }
	);
	check(trace.snapshot(0, 16).events.empty(),
		"disabled trace retained an event");
	check(!trace.begin(0), "zero capacity was accepted");
	check(trace.begin(3), "valid trace capacity was rejected");
	trace.record(
		wt::WtCausalTraceEventKind::StorageStarted,
		wt::WtCausalTraceThreadRole::Storage,
		&first_key,
		{ 8 },
		11,
		12
	);
	trace.record(
		wt::WtCausalTraceEventKind::StorageFinished,
		wt::WtCausalTraceThreadRole::Storage,
		&first_key,
		{ 8 },
		11,
		13,
		900
	);
	trace.record(
		wt::WtCausalTraceEventKind::VisibilityStagingBlocked,
		wt::WtCausalTraceThreadRole::Runtime,
		nullptr,
		{},
		720,
		2532,
		0,
		0
	);
	const wt::WtCausalTraceJobDetails mesh_job {
		wt::WtCausalTraceJobStage::Mesh,
		std::numeric_limits<std::int32_t>::max(),
		91,
		true,
		128,
		127,
		4,
		3,
	};
	trace.record(
		wt::WtCausalTraceEventKind::TransitionMeshFinished,
		wt::WtCausalTraceThreadRole::Runtime,
		&first_key,
		{ 8 },
		1,
		0x12,
		700,
		0,
		&mesh_job
	);
	const wt::WtCausalTraceSnapshot wrapped = trace.snapshot(0, 16);
	check(wrapped.enabled && wrapped.capacity == 3 &&
		wrapped.retained_event_count == 3 &&
		wrapped.dropped_event_count == 2 &&
		wrapped.first_retained_sequence == 2 &&
		wrapped.next_sequence == 5,
		"wrapped trace metadata mismatch");
	check(wrapped.events.size() == 3 &&
		wrapped.events[0].sequence == 2 &&
		wrapped.events[0].kind == wt::WtCausalTraceEventKind::StorageFinished &&
		wrapped.events[0].duration_ns == 900 &&
		wrapped.events[1].kind ==
			wt::WtCausalTraceEventKind::VisibilityStagingBlocked &&
		wrapped.events[1].cause_id == 720 &&
		wrapped.events[1].auxiliary == 2532 &&
		wrapped.events[2].kind ==
			wt::WtCausalTraceEventKind::TransitionMeshFinished &&
		wrapped.events[2].auxiliary == 0x12 &&
		wrapped.events[2].duration_ns == 700 &&
		wrapped.events[2].key == first_key &&
		wrapped.events[2].generation.value == 8 &&
		wrapped.events[2].has_job_details &&
		wrapped.events[2].job.stage == wt::WtCausalTraceJobStage::Mesh &&
		wrapped.events[2].job.effective_priority ==
			std::numeric_limits<std::int32_t>::max() &&
		wrapped.events[2].job.sequence == 91 &&
		wrapped.events[2].job.queue_depth_before == 128 &&
		wrapped.events[2].job.queue_depth_after == 127 &&
		wrapped.events[2].job.jobs_ahead == 4 &&
		wrapped.events[2].job.same_priority_jobs_ahead == 3,
		"wrapped trace event order or identity mismatch");
	const wt::WtCausalTraceSnapshot delta = trace.snapshot(3, 1);
	check(delta.events.size() == 1 && delta.events[0].sequence == 3,
		"delta snapshot mismatch");
	check(trace.snapshot(100, 16).events.empty(),
		"future sequence snapshot was not empty");
	trace.end();
	check(!trace.enabled(), "trace did not disable");
	const wt::WtCausalTraceSnapshot ended = trace.snapshot(0, 16);
	check(ended.next_sequence == 6 &&
		ended.events.back().kind == wt::WtCausalTraceEventKind::TraceStopped,
		"trace stop event was not retained");
	if (failure_count != 0) {
		std::fprintf(stderr, "CAUSAL_TRACE_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("CAUSAL_TRACE_PASS schema=1 bounded_ring=1 disabled_default=1\n");
	return 0;
}
