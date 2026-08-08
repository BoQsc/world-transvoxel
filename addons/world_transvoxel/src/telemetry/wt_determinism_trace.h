#pragma once

#include "core/wt_chunk_state.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

enum class WtDeterminismEventKind : std::uint8_t {
	Requested = 1,
	Sampled = 2,
	Meshed = 3,
	Published = 4,
	Cancelled = 5,
	FailedClosed = 6,
};

struct WtDeterminismTraceEvent {
	WtChunkKey key;
	WtGenerationToken generation;
	WtDeterminismEventKind kind = WtDeterminismEventKind::Requested;
	std::uint64_t authoritative_state_signature = 0;
	std::uint64_t resource_signature = 0;

	bool operator==(const WtDeterminismTraceEvent &other) const noexcept;
};

enum class WtDeterminismDivergenceKind : std::uint8_t {
	None = 0,
	EventMismatch = 1,
	MissingExpectedEvent = 2,
	UnexpectedEvent = 3,
};

struct WtDeterminismTraceComparison {
	WtDeterminismDivergenceKind kind = WtDeterminismDivergenceKind::None;
	std::size_t event_index = 0;
	std::uint64_t first_divergent_generation = 0;
	WtDeterminismTraceEvent expected;
	WtDeterminismTraceEvent actual;

	bool matches() const noexcept;
};

WtDeterminismTraceComparison wt_compare_determinism_traces(
	const std::vector<WtDeterminismTraceEvent> &expected,
	const std::vector<WtDeterminismTraceEvent> &actual
) noexcept;

} // namespace world_transvoxel
