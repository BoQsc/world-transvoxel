#pragma once

#include "core/wt_chunk_state.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

constexpr std::size_t kWtMaximumDesiredViewerCount = 1024;
constexpr std::size_t kWtMaximumDesiredChunkCount = 65536;

struct WtViewerChunkDemand {
	WtChunkKey key;
	std::int32_t priority = 0;
	bool collision_required = false;
};

struct WtDesiredChunk {
	WtChunkKey key;
	std::int32_t priority = 0;
	std::uint32_t supporter_count = 0;
	bool collision_required = false;

	bool operator==(const WtDesiredChunk &other) const noexcept;
};

struct WtDesiredSetDelta {
	std::vector<WtDesiredChunk> added;
	std::vector<WtChunkKey> removed;
	std::vector<WtDesiredChunk> updated;

	void clear() noexcept;
};

struct WtMultiViewerDesiredSetLimits {
	std::size_t viewer_capacity = 8;
	std::size_t demand_capacity_per_viewer = 4096;
	std::size_t total_demand_capacity = 16384;
	std::size_t desired_chunk_capacity = 16384;
};

enum class WtMultiViewerDesiredSetStatus : std::uint8_t {
	Ok,
	InvalidConfiguration,
	InvalidViewer,
	InvalidDemand,
	DuplicateDemand,
	ViewerCapacityExceeded,
	ViewerDemandCapacityExceeded,
	TotalDemandCapacityExceeded,
	DesiredChunkCapacityExceeded,
	StaleViewerRevision,
	ViewerNotFound,
};

struct WtMultiViewerDesiredSetMetrics {
	std::uint64_t viewer_updates = 0;
	std::uint64_t viewer_removals = 0;
	std::uint64_t union_rebuilds = 0;
	std::uint64_t demand_items_processed = 0;
	std::uint64_t chunks_added = 0;
	std::uint64_t chunks_removed = 0;
	std::uint64_t chunks_updated = 0;
	std::uint64_t rejected_events = 0;
};

class WtMultiViewerDesiredSet {
public:
	explicit WtMultiViewerDesiredSet(WtMultiViewerDesiredSetLimits limits);

	bool valid() const noexcept;
	WtMultiViewerDesiredSetStatus update_viewer(
		const WtViewerSnapshot &snapshot,
		const std::vector<WtViewerChunkDemand> &demands,
		WtDesiredSetDelta &delta
	);
	WtMultiViewerDesiredSetStatus remove_viewer(
		std::uint64_t viewer_id,
		std::uint64_t revision,
		WtDesiredSetDelta &delta
	);

	const std::vector<WtDesiredChunk> &get_desired_chunks() const noexcept;
	const WtDesiredChunk *find_desired(const WtChunkKey &key) const noexcept;
	std::size_t viewer_count() const noexcept;
	std::size_t total_demand_count() const noexcept;
	WtMultiViewerDesiredSetMetrics get_metrics() const noexcept;

private:
	struct ViewerRecord {
		WtViewerSnapshot snapshot;
		std::vector<WtViewerChunkDemand> demands;
	};

	WtMultiViewerDesiredSetStatus validate_demands(
		const std::vector<WtViewerChunkDemand> &demands,
		std::vector<WtViewerChunkDemand> &ordered
	) const;
	WtMultiViewerDesiredSetStatus build_union(
		const std::vector<ViewerRecord> &viewers,
		std::vector<WtDesiredChunk> &desired,
		std::uint64_t &processed_count
	) const;
	void build_delta(
		const std::vector<WtDesiredChunk> &candidate,
		WtDesiredSetDelta &delta
	) const;

	WtMultiViewerDesiredSetLimits limits_;
	bool valid_ = false;
	std::size_t total_demand_count_ = 0;
	std::vector<ViewerRecord> viewers_;
	std::vector<WtDesiredChunk> desired_;
	WtMultiViewerDesiredSetMetrics metrics_;
};

} // namespace world_transvoxel
