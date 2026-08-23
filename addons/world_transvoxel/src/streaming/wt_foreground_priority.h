#pragma once

#include "streaming/wt_multi_viewer_desired_set.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace world_transvoxel {

constexpr std::size_t kWtForegroundPrioritySourceCapacity = 8;
constexpr std::size_t kWtForegroundPriorityKeysPerSource = 64;

constexpr std::int32_t kWtCommittedEditPriority =
	std::numeric_limits<std::int32_t>::max();
constexpr std::int32_t kWtPlayerSupportPriority =
	std::numeric_limits<std::int32_t>::max() - 1;
// Every collision-planner priority remains above pre-click interaction focus.
constexpr std::int32_t kWtInteractionFocusPriority =
	std::numeric_limits<std::int32_t>::max() - 1000002;

enum class WtForegroundPriorityClass : std::uint8_t {
	PlayerSupport,
	InteractionFocus,
};

enum class WtForegroundPriorityStatus : std::uint8_t {
	Ok,
	InvalidRequest,
	StaleRevision,
	SourceCapacityExceeded,
};

struct WtForegroundPriorityLeaseRequest {
	std::uint64_t source_id = 0;
	std::uint64_t revision = 0;
	WtForegroundPriorityClass priority_class =
		WtForegroundPriorityClass::InteractionFocus;
	std::vector<WtChunkKey> keys;
};

struct WtForegroundPriorityOverlayResult {
	std::size_t requested_keys = 0;
	std::size_t matched_keys = 0;
	std::size_t missing_keys = 0;
	std::size_t changed_priorities = 0;
};

class WtForegroundPriorityLeaseSet {
public:
	WtForegroundPriorityStatus update(
		const WtForegroundPriorityLeaseRequest &request
	);
	WtForegroundPriorityOverlayResult apply(
		const std::vector<WtViewerChunkDemand> &base,
		std::vector<WtViewerChunkDemand> &effective
	) const;

	std::size_t source_count() const noexcept;
	std::size_t active_source_count() const noexcept;
	std::size_t active_key_count(
		WtForegroundPriorityClass priority_class
	) const noexcept;

private:
	struct SourceRecord {
		std::uint64_t source_id = 0;
		std::uint64_t revision = 0;
		WtForegroundPriorityClass priority_class =
			WtForegroundPriorityClass::InteractionFocus;
		std::vector<WtChunkKey> keys;
	};

	std::vector<SourceRecord> sources_;
};

std::int32_t wt_foreground_priority(
	WtForegroundPriorityClass priority_class
) noexcept;

} // namespace world_transvoxel
