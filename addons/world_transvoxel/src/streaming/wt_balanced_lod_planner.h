#pragma once

#include "physics/wt_collision_builder.h"
#include "streaming/wt_lod_map.h"
#include "streaming/wt_multi_viewer_desired_set.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

constexpr std::uint32_t kWtLodRefinementHysteresisChunks = 1;

struct WtLodPlannerViewer {
	WtViewerSnapshot snapshot;
	std::uint32_t radius_chunks = 0;
	std::uint8_t maximum_lod = 0;
	std::uint32_t refinement_radius_chunks = 0;
};

struct WtBalancedLodPlan {
	std::vector<WtViewerChunkDemand> demands;
	std::vector<WtLodMapEntry> entries;

	void clear() noexcept;
};

enum class WtBalancedLodPlannerStatus : std::uint8_t {
	Ok,
	InvalidConfiguration,
	InvalidViewer,
	DuplicateViewer,
	CapacityExceeded,
	IncompleteHierarchy,
	InvalidLodMap,
};

class WtBalancedLodPlanner {
public:
	WtBalancedLodPlanner(
		std::size_t active_capacity,
		std::vector<WtChunkKey> page_catalog,
		std::uint32_t refinement_radius_limit_chunks = 0,
		bool global_coarse_lod_coverage = false
	);

	bool valid() const noexcept;
	WtBalancedLodPlannerStatus plan(
		const std::vector<WtLodPlannerViewer> &viewers,
		const std::vector<WtDesiredChunk> &current_desired,
		const WtCollisionPolicy &collision_policy,
		WtBalancedLodPlan &output
	) const;

	std::size_t active_capacity() const noexcept;
	std::size_t catalog_size() const noexcept;

private:
	bool catalog_contains(const WtChunkKey &key) const noexcept;
	bool append_subtree(
		const WtChunkKey &key,
		const std::vector<WtLodPlannerViewer> &viewers,
		const std::vector<WtChunkKey> &refined_ancestors,
		std::vector<WtChunkKey> &leaves
	) const;
	bool should_refine(
		const WtChunkKey &key,
		const std::vector<WtLodPlannerViewer> &viewers,
		const std::vector<WtChunkKey> &refined_ancestors
	) const noexcept;
	WtBalancedLodPlannerStatus refine_leaf(
		std::vector<WtChunkKey> &leaves,
		std::size_t leaf_index
	) const;
	WtBalancedLodPlannerStatus balance(
		std::vector<WtChunkKey> &leaves,
		WtLodMap &lod_map
	) const;

	std::size_t active_capacity_ = 0;
	std::uint32_t refinement_radius_limit_chunks_ = 0;
	std::vector<WtChunkKey> page_catalog_;
	bool global_coarse_lod_coverage_ = false;
	bool valid_ = false;
};

const char *wt_balanced_lod_planner_status_message(
	WtBalancedLodPlannerStatus status
) noexcept;

} // namespace world_transvoxel
