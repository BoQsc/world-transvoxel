#include "telemetry/wt_causal_trace.h"

#include "telemetry/wt_runtime_trace.h"

#include <algorithm>
#include <chrono>

namespace world_transvoxel {

std::uint64_t wt_causal_trace_now_ns() noexcept {
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()
		).count()
	);
}

bool WtCausalTraceBuffer::begin(std::size_t capacity) {
	if (capacity == 0 || capacity > kWtMaximumRuntimeTraceEvents) {
		return false;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	enabled_.store(false, std::memory_order_release);
	slots_.clear();
	slots_.resize(capacity);
	write_index_ = 0;
	event_count_ = 0;
	next_sequence_ = 0;
	dropped_event_count_ = 0;
	started_ns_ = wt_causal_trace_now_ns();
	WtCausalTraceEvent started;
	started.sequence = next_sequence_++;
	started.kind = WtCausalTraceEventKind::TraceStarted;
	started.thread_role = WtCausalTraceThreadRole::Api;
	slots_[write_index_] = started;
	write_index_ = (write_index_ + 1U) % slots_.size();
	++event_count_;
	enabled_.store(true, std::memory_order_release);
	return true;
}

void WtCausalTraceBuffer::end() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!enabled_.load(std::memory_order_relaxed) || slots_.empty()) return;
	WtCausalTraceEvent stopped;
	stopped.sequence = next_sequence_++;
	stopped.elapsed_ns = wt_causal_trace_now_ns() - started_ns_;
	stopped.kind = WtCausalTraceEventKind::TraceStopped;
	stopped.thread_role = WtCausalTraceThreadRole::Api;
	slots_[write_index_] = stopped;
	write_index_ = (write_index_ + 1U) % slots_.size();
	if (event_count_ < slots_.size()) {
		++event_count_;
	} else {
		++dropped_event_count_;
	}
	enabled_.store(false, std::memory_order_release);
}

void WtCausalTraceBuffer::clear() {
	std::lock_guard<std::mutex> lock(mutex_);
	write_index_ = 0;
	event_count_ = 0;
	next_sequence_ = 0;
	dropped_event_count_ = 0;
	started_ns_ = wt_causal_trace_now_ns();
}

bool WtCausalTraceBuffer::enabled() const noexcept {
	return enabled_.load(std::memory_order_acquire);
}

void WtCausalTraceBuffer::record(
	WtCausalTraceEventKind kind,
	WtCausalTraceThreadRole thread_role,
	const WtChunkKey *key,
	WtGenerationToken generation,
	std::uint64_t cause_id,
	std::uint64_t auxiliary,
	std::uint64_t duration_ns,
	std::int64_t status
) {
	if (!enabled_.load(std::memory_order_acquire)) return;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!enabled_.load(std::memory_order_relaxed) || slots_.empty()) return;
	WtCausalTraceEvent event;
	event.sequence = next_sequence_++;
	event.elapsed_ns = wt_causal_trace_now_ns() - started_ns_;
	event.duration_ns = duration_ns;
	event.kind = kind;
	event.thread_role = thread_role;
	event.has_chunk = key != nullptr;
	if (key != nullptr) event.key = *key;
	event.generation = generation;
	event.cause_id = cause_id;
	event.auxiliary = auxiliary;
	event.status = status;
	slots_[write_index_] = event;
	write_index_ = (write_index_ + 1U) % slots_.size();
	if (event_count_ < slots_.size()) {
		++event_count_;
	} else {
		++dropped_event_count_;
	}
}

WtCausalTraceSnapshot WtCausalTraceBuffer::snapshot(
	std::uint64_t first_sequence,
	std::size_t maximum_events
) const {
	std::lock_guard<std::mutex> lock(mutex_);
	WtCausalTraceSnapshot output;
	output.enabled = enabled_.load(std::memory_order_relaxed);
	output.capacity = slots_.size();
	output.retained_event_count = event_count_;
	output.dropped_event_count = dropped_event_count_;
	output.next_sequence = next_sequence_;
	output.first_retained_sequence = next_sequence_ - event_count_;
	if (slots_.empty() || event_count_ == 0 || maximum_events == 0) {
		return output;
	}
	const std::uint64_t begin = std::min(
		next_sequence_,
		std::max(first_sequence, output.first_retained_sequence)
	);
	const std::uint64_t available = next_sequence_ - begin;
	const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(
		available,
		maximum_events
	));
	output.events.reserve(count);
	const std::size_t oldest =
		(write_index_ + slots_.size() - event_count_) % slots_.size();
	for (std::size_t index = 0; index < count; ++index) {
		const std::uint64_t sequence = begin + index;
		const std::size_t retained_offset = static_cast<std::size_t>(
			sequence - output.first_retained_sequence
		);
		output.events.push_back(
			slots_[(oldest + retained_offset) % slots_.size()]
		);
	}
	return output;
}

const char *wt_causal_trace_event_kind_name(
	WtCausalTraceEventKind kind
) noexcept {
	switch (kind) {
		case WtCausalTraceEventKind::TraceStarted: return "trace_started";
		case WtCausalTraceEventKind::TraceStopped: return "trace_stopped";
		case WtCausalTraceEventKind::ViewerPlanStarted: return "viewer_plan_started";
		case WtCausalTraceEventKind::ViewerPlanApplied: return "viewer_plan_applied";
		case WtCausalTraceEventKind::ChunkDemandAccepted: return "chunk_demand_accepted";
		case WtCausalTraceEventKind::EditSubmitted: return "edit_submitted";
		case WtCausalTraceEventKind::EditProcessingStarted: return "edit_processing_started";
		case WtCausalTraceEventKind::EditCommitted: return "edit_committed";
		case WtCausalTraceEventKind::EditRejected: return "edit_rejected";
		case WtCausalTraceEventKind::StorageRequested: return "storage_requested";
		case WtCausalTraceEventKind::StorageStarted: return "storage_started";
		case WtCausalTraceEventKind::StorageFinished: return "storage_finished";
		case WtCausalTraceEventKind::StorageCompletionConsumed: return "storage_completion_consumed";
		case WtCausalTraceEventKind::SampleStarted: return "sample_started";
		case WtCausalTraceEventKind::SampleFinished: return "sample_finished";
		case WtCausalTraceEventKind::MeshStarted: return "mesh_started";
		case WtCausalTraceEventKind::MeshFinished: return "mesh_finished";
		case WtCausalTraceEventKind::MeshCompletionConsumed: return "mesh_completion_consumed";
		case WtCausalTraceEventKind::TransitionMeshStarted: return "transition_mesh_started";
		case WtCausalTraceEventKind::TransitionMeshFinished: return "transition_mesh_finished";
		case WtCausalTraceEventKind::TransitionMeshCompletionConsumed: return "transition_mesh_completion_consumed";
		case WtCausalTraceEventKind::PublicationQueued: return "publication_queued";
		case WtCausalTraceEventKind::PublicationPopped: return "publication_popped";
		case WtCausalTraceEventKind::FrontendPublicationProcessed: return "frontend_publication_processed";
		case WtCausalTraceEventKind::RenderSinkApplied: return "render_sink_applied";
		case WtCausalTraceEventKind::CollisionSinkApplied: return "collision_sink_applied";
		case WtCausalTraceEventKind::VisibilityReplacementReady: return "visibility_replacement_ready";
		case WtCausalTraceEventKind::VisibilityStagingBlocked: return "visibility_staging_blocked";
		case WtCausalTraceEventKind::VisibilityBatchPublished: return "visibility_batch_published";
		case WtCausalTraceEventKind::VisibilityCoveragePriorityRequested:
			return "visibility_coverage_priority_requested";
		case WtCausalTraceEventKind::VisibilityCoveragePriorityApplied:
			return "visibility_coverage_priority_applied";
		case WtCausalTraceEventKind::VisibilityRegionReplacementMember:
			return "visibility_region_replacement_member";
		case WtCausalTraceEventKind::VisibilityRegionRetirementMember:
			return "visibility_region_retirement_member";
	}
	return "unknown";
}

const char *wt_causal_trace_thread_role_name(
	WtCausalTraceThreadRole role
) noexcept {
	switch (role) {
		case WtCausalTraceThreadRole::Api: return "api";
		case WtCausalTraceThreadRole::Runtime: return "runtime";
		case WtCausalTraceThreadRole::Storage: return "storage";
		case WtCausalTraceThreadRole::Frontend: return "frontend";
	}
	return "unknown";
}

} // namespace world_transvoxel
