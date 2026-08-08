#include "telemetry/wt_determinism_trace.h"

#include <algorithm>

namespace world_transvoxel {

bool WtDeterminismTraceEvent::operator==(
	const WtDeterminismTraceEvent &other
) const noexcept {
	return key == other.key && generation == other.generation &&
		kind == other.kind &&
		authoritative_state_signature == other.authoritative_state_signature &&
		resource_signature == other.resource_signature;
}

bool WtDeterminismTraceComparison::matches() const noexcept {
	return kind == WtDeterminismDivergenceKind::None;
}

WtDeterminismTraceComparison wt_compare_determinism_traces(
	const std::vector<WtDeterminismTraceEvent> &expected,
	const std::vector<WtDeterminismTraceEvent> &actual
) noexcept {
	WtDeterminismTraceComparison output;
	const std::size_t common_count = std::min(expected.size(), actual.size());
	for (std::size_t index = 0; index < common_count; ++index) {
		if (expected[index] == actual[index]) continue;
		output.kind = WtDeterminismDivergenceKind::EventMismatch;
		output.event_index = index;
		output.expected = expected[index];
		output.actual = actual[index];
		output.first_divergent_generation = std::min(
			expected[index].generation.value,
			actual[index].generation.value
		);
		return output;
	}
	if (expected.size() > common_count) {
		output.kind = WtDeterminismDivergenceKind::MissingExpectedEvent;
		output.event_index = common_count;
		output.expected = expected[common_count];
		output.first_divergent_generation = output.expected.generation.value;
	} else if (actual.size() > common_count) {
		output.kind = WtDeterminismDivergenceKind::UnexpectedEvent;
		output.event_index = common_count;
		output.actual = actual[common_count];
		output.first_divergent_generation = output.actual.generation.value;
	}
	return output;
}

} // namespace world_transvoxel
