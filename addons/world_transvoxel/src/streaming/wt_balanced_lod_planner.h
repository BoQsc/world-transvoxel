#pragma once

#include "physics/wt_collision_builder.h"
#include "storage/wt_page_hierarchy.h"
#include "streaming/wt_lod_map.h"
#include "streaming/wt_multi_viewer_desired_set.h"

#include <cstddef>
#include <cstdint>
#include <functional>
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
	Cancelled,
};

class WtBalancedLodPlanner {
public:
	WtBalancedLodPlanner(
		std::size_t active_capacity,
		std::vector<WtChunkKey> page_catalog,
		std::uint32_t refinement_radius_limit_chunks = 0,
		bool global_coarse_lod_coverage = false
	);
	WtBalancedLodPlanner(
		std::size_t active_capacity,
		WtPageHierarchy page_hierarchy,
		std::uint32_t refinement_radius_limit_chunks = 0,
		bool global_coarse_lod_coverage = false
	);

	bool valid() const noexcept;
	WtBalancedLodPlannerStatus plan(
		const std::vector<WtLodPlannerViewer> &viewers,
		const std::vector<WtDesiredChunk> &current_desired,
		const WtCollisionPolicy &collision_policy,
		WtBalancedLodPlan &output,
		bool visual_viewer_collision_enabled = true,
		const std::function<bool()> &cancel_requested = {},
		const std::vector<WtChunkKey> &forced_leaf_keys = {}
	) const;
	WtBalancedLodPlannerStatus stage_toward(
		const WtBalancedLodPlan &target,
		const WtBalancedLodPlan &current,
		const std::vector<WtChunkKey> &visually_ready,
		std::uint8_t staging_root_lod,
		std::size_t maximum_topology_changes,
		WtBalancedLodPlan &output,
		bool &complete,
		const std::vector<WtChunkKey> &preferred_refinement_keys = {},
		bool preferred_refinement_only = false,
		bool allow_unready_preferred_refinement = false,
		bool allow_preferred_coarsening = false,
		const std::function<bool()> &cancel_requested = {}
	) const;

	std::size_t active_capacity() const noexcept;
	WtBalancedLodPlannerStatus stage_foreground(
		const WtBalancedLodPlan &target, const WtBalancedLodPlan &current,
		const std::vector<WtChunkKey> &visually_ready, std::uint8_t staging_root_lod,
		const std::vector<WtChunkKey> &foreground_keys,
		WtBalancedLodPlan &output, bool &complete,
		const std::function<bool()> &cancel_requested = {}
	) const;
	std::size_t catalog_size() const noexcept;
	WtPageHierarchyMetrics hierarchy_metrics() const noexcept;

private:
	bool catalog_contains(const WtChunkKey &key) const noexcept;
	WtBalancedLodPlannerStatus append_subtree(
		const WtChunkKey &key,
		const std::vector<WtLodPlannerViewer> &viewers,
		const std::vector<WtChunkKey> &refined_ancestors,
		const std::vector<WtChunkKey> &forced_leaf_keys,
		std::vector<WtChunkKey> &leaves,
		const std::function<bool()> &cancel_requested
	) const;
	bool should_refine(
		const WtChunkKey &key,
		const std::vector<WtLodPlannerViewer> &viewers,
		const std::vector<WtChunkKey> &refined_ancestors,
		const std::vector<WtChunkKey> &forced_leaf_keys
	) const noexcept;
	WtBalancedLodPlannerStatus refine_leaf(
		std::vector<WtChunkKey> &leaves,
		std::size_t leaf_index
	) const;
	WtBalancedLodPlannerStatus balance(
		std::vector<WtChunkKey> &leaves,
		WtLodMap &lod_map,
		const std::function<bool()> &cancel_requested = {}
	) const;

	std::size_t active_capacity_ = 0;
	std::uint32_t refinement_radius_limit_chunks_ = 0;
	WtPageHierarchy page_hierarchy_;
	bool global_coarse_lod_coverage_ = false;
	bool valid_ = false;
};

const char *wt_balanced_lod_planner_status_message(
	WtBalancedLodPlannerStatus status
) noexcept;

} // namespace world_transvoxel
