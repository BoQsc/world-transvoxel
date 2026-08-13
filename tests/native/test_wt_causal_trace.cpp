#include "telemetry/wt_causal_trace.h"

#include <cstdio>

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
	const wt::WtCausalTraceSnapshot wrapped = trace.snapshot(0, 16);
	check(wrapped.enabled && wrapped.capacity == 3 &&
		wrapped.retained_event_count == 3 &&
		wrapped.dropped_event_count == 1 &&
		wrapped.first_retained_sequence == 1 &&
		wrapped.next_sequence == 4,
		"wrapped trace metadata mismatch");
	check(wrapped.events.size() == 3 &&
		wrapped.events[0].sequence == 1 &&
		wrapped.events[1].duration_ns == 900 &&
		wrapped.events[2].kind ==
			wt::WtCausalTraceEventKind::VisibilityStagingBlocked &&
		wrapped.events[2].cause_id == 720 &&
		wrapped.events[2].auxiliary == 2532 &&
		wrapped.events[0].key == first_key &&
		wrapped.events[0].generation.value == 8,
		"wrapped trace event order or identity mismatch");
	const wt::WtCausalTraceSnapshot delta = trace.snapshot(3, 1);
	check(delta.events.size() == 1 && delta.events[0].sequence == 3,
		"delta snapshot mismatch");
	check(trace.snapshot(100, 16).events.empty(),
		"future sequence snapshot was not empty");
	trace.end();
	check(!trace.enabled(), "trace did not disable");
	const wt::WtCausalTraceSnapshot ended = trace.snapshot(0, 16);
	check(ended.next_sequence == 5 &&
		ended.events.back().kind == wt::WtCausalTraceEventKind::TraceStopped,
		"trace stop event was not retained");
	if (failure_count != 0) {
		std::fprintf(stderr, "CAUSAL_TRACE_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("CAUSAL_TRACE_PASS schema=1 bounded_ring=1 disabled_default=1\n");
	return 0;
}
