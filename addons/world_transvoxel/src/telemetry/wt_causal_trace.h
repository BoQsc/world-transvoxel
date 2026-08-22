#pragma once

#include "core/wt_chunk_state.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace world_transvoxel {

enum class WtCausalTraceEventKind : std::uint16_t {
	TraceStarted = 1,
	TraceStopped,
	ViewerPlanStarted,
	ViewerPlanApplied,
	ChunkDemandAccepted,
	EditSubmitted,
	EditProcessingStarted,
	EditCommitted,
	EditRejected,
	StorageRequested,
	StorageStarted,
	StorageFinished,
	StorageCompletionConsumed,
	SampleStarted,
	SampleFinished,
	MeshStarted,
	MeshFinished,
	MeshCompletionConsumed,
	TransitionMeshStarted,
	TransitionMeshFinished,
	TransitionMeshCompletionConsumed,
	PublicationQueued,
	PublicationPopped,
	FrontendPublicationProcessed,
	RenderSinkApplied,
	CollisionSinkApplied,
	VisibilityReplacementReady,
	VisibilityStagingBlocked,
	VisibilityBatchPublished,
	VisibilityCoveragePriorityRequested,
	VisibilityCoveragePriorityApplied,
	VisibilityRegionReplacementMember,
	VisibilityRegionRetirementMember,
};

enum class WtCausalTraceThreadRole : std::uint8_t {
	Api = 1,
	Runtime,
	Storage,
	Frontend,
};

struct WtCausalTraceEvent {
	std::uint64_t sequence = 0;
	std::uint64_t elapsed_ns = 0;
	std::uint64_t duration_ns = 0;
	WtCausalTraceEventKind kind = WtCausalTraceEventKind::TraceStarted;
	WtCausalTraceThreadRole thread_role = WtCausalTraceThreadRole::Api;
	bool has_chunk = false;
	WtChunkKey key;
	WtGenerationToken generation;
	std::uint64_t cause_id = 0;
	std::uint64_t auxiliary = 0;
	std::int64_t status = 0;
};

struct WtCausalTraceSnapshot {
	bool enabled = false;
	std::size_t capacity = 0;
	std::size_t retained_event_count = 0;
	std::uint64_t dropped_event_count = 0;
	std::uint64_t first_retained_sequence = 0;
	std::uint64_t next_sequence = 0;
	std::vector<WtCausalTraceEvent> events;
};

class WtCausalTraceBuffer {
public:
	bool begin(std::size_t capacity);
	void end();
	void clear();
	bool enabled() const noexcept;
	void record(
		WtCausalTraceEventKind kind,
		WtCausalTraceThreadRole thread_role,
		const WtChunkKey *key = nullptr,
		WtGenerationToken generation = {},
		std::uint64_t cause_id = 0,
		std::uint64_t auxiliary = 0,
		std::uint64_t duration_ns = 0,
		std::int64_t status = 0
	);
	WtCausalTraceSnapshot snapshot(
		std::uint64_t first_sequence,
		std::size_t maximum_events
	) const;

private:
	mutable std::mutex mutex_;
	std::atomic<bool> enabled_{ false };
	std::vector<WtCausalTraceEvent> slots_;
	std::size_t write_index_ = 0;
	std::size_t event_count_ = 0;
	std::uint64_t next_sequence_ = 0;
	std::uint64_t dropped_event_count_ = 0;
	std::uint64_t started_ns_ = 0;
};

const char *wt_causal_trace_event_kind_name(
	WtCausalTraceEventKind kind
) noexcept;
const char *wt_causal_trace_thread_role_name(
	WtCausalTraceThreadRole role
) noexcept;
std::uint64_t wt_causal_trace_now_ns() noexcept;

} // namespace world_transvoxel
