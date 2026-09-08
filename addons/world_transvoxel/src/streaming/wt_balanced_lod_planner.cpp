#include "streaming/wt_balanced_lod_planner.h"

#include "services/wt_runtime_config.h"
#include "storage/wt_world_manifest.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace world_transvoxel {
namespace {

constexpr std::int32_t kWtLodPriorityBand = 100000000;
constexpr std::int32_t kWtDistancePriorityMaximum =
	kWtLodPriorityBand - 1;
constexpr std::int32_t kWtCollisionPriorityMaximum =
	std::numeric_limits<std::int32_t>::max() - 1;
constexpr std::int32_t kWtCollisionPriorityDistanceLimit = 1000000;
constexpr std::int32_t kWtMaximumVisualPriority =
	static_cast<std::int32_t>(kWtMaximumLod) * kWtLodPriorityBand +
	kWtDistancePriorityMaximum;
static_assert(
	kWtCollisionPriorityMaximum - kWtCollisionPriorityDistanceLimit >
		kWtMaximumVisualPriority,
	"collision priority band must outrank every visual LOD priority"
);

bool adjacent_key(
	const WtChunkKey &key,
	WtChunkFace face,
	WtChunkKey &adjacent
) noexcept {
	adjacent = key;
	std::int32_t *axis = nullptr;
	bool positive = false;
	switch (face) {
		case WtChunkFace::NegativeX: axis = &adjacent.x; break;
		case WtChunkFace::PositiveX: axis = &adjacent.x; positive = true; break;
		case WtChunkFace::NegativeY: axis = &adjacent.y; break;
		case WtChunkFace::PositiveY: axis = &adjacent.y; positive = true; break;
		case WtChunkFace::NegativeZ: axis = &adjacent.z; break;
		case WtChunkFace::PositiveZ: axis = &adjacent.z; positive = true; break;
	}
	if ((positive && *axis == std::numeric_limits<std::int32_t>::max()) ||
		(!positive && *axis == std::numeric_limits<std::int32_t>::min())) {
		return false;
	}
	*axis += positive ? 1 : -1;
	return true;
}

std::size_t find_key_index(
	const std::vector<WtChunkKey> &keys,
	const WtChunkKey &key
) noexcept {
	const auto iterator = std::lower_bound(keys.begin(), keys.end(), key);
	return iterator != keys.end() && *iterator == key ?
		static_cast<std::size_t>(iterator - keys.begin()) : keys.size();
}

const WtLodMapEntry *find_map_entry(
	const std::vector<WtLodMapEntry> &entries,
	const WtChunkKey &key
) noexcept {
	const auto iterator = std::lower_bound(
		entries.begin(), entries.end(), key,
		[](const WtLodMapEntry &entry, const WtChunkKey &value) {
			return entry.key < value;
		}
	);
	return iterator != entries.end() && iterator->key == key ?
		&*iterator : nullptr;
}

bool borders_coarser_leaf(
	const WtChunkKey &leaf,
	const std::vector<WtLodMapEntry> &entries
) noexcept {
	for (std::uint8_t face_index = 0; face_index < 6U; ++face_index) {
		WtChunkKey neighbor;
		if (!adjacent_key(
				leaf, static_cast<WtChunkFace>(face_index), neighbor
			)) continue;
		while (neighbor.lod < kWtMaximumLod) {
			neighbor = wt_parent_chunk_key(neighbor);
			if (find_map_entry(entries, neighbor) != nullptr) return true;
		}
	}
	return false;
}

bool plan_covers_key(
	const std::vector<WtLodMapEntry> &entries,
	WtChunkKey key
) noexcept {
	for (;;) {
		if (find_map_entry(entries, key) != nullptr) return true;
		if (key.lod == kWtMaximumLod) return false;
		key = wt_parent_chunk_key(key);
	}
}

struct TargetDescendantSummary {
	WtChunkKey ancestor;
	std::uint8_t deepest_lod = kWtMaximumLod;
	std::int32_t background_priority = std::numeric_limits<std::int32_t>::min();
	std::int32_t deepest_priority = std::numeric_limits<std::int32_t>::min();
};

std::vector<TargetDescendantSummary> summarize_target_descendants(
	const std::vector<WtViewerChunkDemand> &demands
) {
	std::vector<TargetDescendantSummary> summaries;
	summaries.reserve(demands.size() * 2U);
	for (const WtViewerChunkDemand &demand : demands) {
		WtChunkKey ancestor = demand.key;
		while (ancestor.lod < kWtMaximumLod) {
			ancestor = wt_parent_chunk_key(ancestor);
			summaries.push_back({
				ancestor, demand.key.lod, demand.priority, demand.priority
			});
		}
	}
	std::sort(summaries.begin(), summaries.end(), [](const auto &left, const auto &right) {
		return left.ancestor < right.ancestor;
	});
	std::size_t write = 0;
	for (const TargetDescendantSummary &summary : summaries) {
		if (write != 0 && summaries[write - 1U].ancestor == summary.ancestor) {
			TargetDescendantSummary &merged = summaries[write - 1U];
			merged.background_priority = std::max(
				merged.background_priority, summary.background_priority
			);
			if (summary.deepest_lod < merged.deepest_lod) {
				merged.deepest_lod = summary.deepest_lod;
				merged.deepest_priority = summary.deepest_priority;
			} else if (summary.deepest_lod == merged.deepest_lod) {
				merged.deepest_priority = std::max(
					merged.deepest_priority, summary.deepest_priority
				);
			}
			continue;
		}
		summaries[write++] = summary;
	}
	summaries.resize(write);
	return summaries;
}

const TargetDescendantSummary *find_target_descendants(
	const std::vector<TargetDescendantSummary> &summaries,
	const WtChunkKey &key
) noexcept {
	const auto iterator = std::lower_bound(
		summaries.begin(), summaries.end(), key,
		[](const TargetDescendantSummary &summary, const WtChunkKey &value) {
			return summary.ancestor < value;
		}
	);
	return iterator != summaries.end() && iterator->ancestor == key ?
		&*iterator : nullptr;
}

std::size_t find_unbalanced_coarse_leaf(
	const std::vector<WtChunkKey> &leaves
) noexcept {
	for (const WtChunkKey &leaf : leaves) {
		for (std::uint8_t face_index = 0; face_index < 6U; ++face_index) {
			WtChunkKey neighbor;
			if (!adjacent_key(
					leaf, static_cast<WtChunkFace>(face_index), neighbor
				)) continue;
			while (neighbor.lod < kWtMaximumLod) {
				neighbor = wt_parent_chunk_key(neighbor);
				const std::size_t index = find_key_index(leaves, neighbor);
				if (index == leaves.size()) continue;
				if (neighbor.lod - leaf.lod > 1U) return index;
				break;
			}
		}
	}
	return leaves.size();
}

double axis_distance(double point, double minimum, double maximum) noexcept {
	if (point < minimum) return minimum - point;
	if (point > maximum) return point - maximum;
	return 0.0;
}

double distance_to_chunk(
	const WtViewerSnapshot &viewer,
	const WtChunkKey &key
) noexcept {
	const WtChunkBounds bounds = wt_chunk_bounds(key);
	const double dx = axis_distance(viewer.x, bounds.minimum.x, bounds.maximum.x);
	const double dy = axis_distance(viewer.y, bounds.minimum.y, bounds.maximum.y);
	const double dz = axis_distance(viewer.z, bounds.minimum.z, bounds.maximum.z);
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool chunk_coordinate(
	double position,
	std::uint8_t lod,
	std::int32_t &coordinate
) noexcept {
	if (!std::isfinite(position)) return false;
	const double value = std::floor(
		position / static_cast<double>(wt_chunk_extent(lod))
	);
	if (value < std::numeric_limits<std::int32_t>::min() ||
		value > std::numeric_limits<std::int32_t>::max()) {
		return false;
	}
	coordinate = static_cast<std::int32_t>(value);
	return true;
}

const WtDesiredChunk *find_current(
	const std::vector<WtDesiredChunk> &current,
	const WtChunkKey &key
) noexcept {
	const auto iterator = std::lower_bound(
		current.begin(), current.end(), key,
		[](const WtDesiredChunk &item, const WtChunkKey &value) {
			return item.key < value;
		}
	);
	return iterator != current.end() && iterator->key == key ? &*iterator :
		nullptr;
}

bool bounded_radius(std::uint32_t radius) noexcept {
	const std::uint64_t width = static_cast<std::uint64_t>(radius) * 2U + 1U;
	return width <= kWtMaximumDesiredChunkCount &&
		width <= kWtMaximumDesiredChunkCount / width &&
		width * width <= kWtMaximumDesiredChunkCount / width;
}

bool bounds_contain(
	const WtChunkKey &outer,
	const WtChunkKey &inner
) noexcept {
	const WtChunkBounds outer_bounds = wt_chunk_bounds(outer);
	const WtChunkBounds inner_bounds = wt_chunk_bounds(inner);
	return outer_bounds.minimum.x <= inner_bounds.minimum.x &&
		outer_bounds.minimum.y <= inner_bounds.minimum.y &&
		outer_bounds.minimum.z <= inner_bounds.minimum.z &&
		outer_bounds.maximum.x >= inner_bounds.maximum.x &&
		outer_bounds.maximum.y >= inner_bounds.maximum.y &&
		outer_bounds.maximum.z >= inner_bounds.maximum.z;
}

bool keys_overlap(
	const WtChunkKey &left,
	const WtChunkKey &right
) noexcept {
	return bounds_contain(left, right) || bounds_contain(right, left);
}

const WtViewerChunkDemand *find_demand(
	const std::vector<WtViewerChunkDemand> &demands,
	const WtChunkKey &key
) noexcept {
	const auto iterator = std::lower_bound(
		demands.begin(), demands.end(), key,
		[](const WtViewerChunkDemand &item, const WtChunkKey &value) {
			return item.key < value;
		}
	);
	return iterator != demands.end() && iterator->key == key ? &*iterator :
		nullptr;
}

bool append_staged_demand(
	const WtChunkKey &key,
	const WtBalancedLodPlan &target,
	const WtBalancedLodPlan &current,
	std::vector<WtViewerChunkDemand> &output
) {
	if (const WtViewerChunkDemand *exact = find_demand(target.demands, key)) {
		output.push_back(*exact);
		return true;
	}
	bool found = false;
	WtViewerChunkDemand staged;
	staged.key = key;
	staged.priority = std::numeric_limits<std::int32_t>::min();
	for (const WtViewerChunkDemand &demand : target.demands) {
		if (!keys_overlap(key, demand.key)) continue;
		found = true;
		staged.priority = std::max(staged.priority, demand.priority);
		staged.collision_required =
			staged.collision_required || demand.collision_required;
		staged.visual_required =
			staged.visual_required || demand.visual_required;
	}
	if (!found) {
		const WtViewerChunkDemand *retained = find_demand(current.demands, key);
		if (retained == nullptr) return false;
		staged = *retained;
	}
	output.push_back(staged);
	return true;
}

bool same_lod_plan(
	const WtBalancedLodPlan &left,
	const WtBalancedLodPlan &right
) noexcept {
	if (left.entries.size() != right.entries.size()) return false;
	for (std::size_t index = 0; index < left.entries.size(); ++index) {
		if (left.entries[index].key != right.entries[index].key ||
			left.entries[index].transition_mask !=
				right.entries[index].transition_mask) {
			return false;
		}
	}
	return true;
}

} // namespace

void WtBalancedLodPlan::clear() noexcept {
	demands.clear();
	entries.clear();
}

WtBalancedLodPlanner::WtBalancedLodPlanner(
	std::size_t active_capacity,
	std::vector<WtChunkKey> page_catalog,
	std::uint32_t refinement_radius_limit_chunks,
	bool global_coarse_lod_coverage
) : WtBalancedLodPlanner(
		active_capacity,
		WtPageHierarchy::explicit_catalog(std::move(page_catalog)),
		refinement_radius_limit_chunks,
		global_coarse_lod_coverage
	) {
}

WtBalancedLodPlanner::WtBalancedLodPlanner(
	std::size_t active_capacity,
	WtPageHierarchy page_hierarchy,
	std::uint32_t refinement_radius_limit_chunks,
	bool global_coarse_lod_coverage
) :
		active_capacity_(active_capacity),
		refinement_radius_limit_chunks_(refinement_radius_limit_chunks),
		global_coarse_lod_coverage_(global_coarse_lod_coverage),
		page_hierarchy_(std::move(page_hierarchy)) {
	valid_ = active_capacity_ != 0 &&
		active_capacity_ <= kWtMaximumRuntimeActiveChunks &&
		page_hierarchy_.valid() &&
		page_hierarchy_.page_count() <= kWtMaximumWorldPageCount;
}

bool WtBalancedLodPlanner::valid() const noexcept {
	return valid_;
}

bool WtBalancedLodPlanner::catalog_contains(
	const WtChunkKey &key
) const noexcept {
	return page_hierarchy_.contains(key);
}

bool WtBalancedLodPlanner::should_refine(
	const WtChunkKey &key,
	const std::vector<WtLodPlannerViewer> &viewers,
	const std::vector<WtChunkKey> &refined_ancestors,
	const std::vector<WtChunkKey> &forced_leaf_keys
) const noexcept {
	if (key.lod == 0) return false;
	if (std::any_of(
			forced_leaf_keys.begin(), forced_leaf_keys.end(),
			[&key](const WtChunkKey &leaf) {
				return key.lod > leaf.lod && bounds_contain(key, leaf);
			}
		)) {
		return true;
	}
	const double child_extent = static_cast<double>(wt_chunk_extent(key.lod - 1));
	const bool was_refined = std::binary_search(
		refined_ancestors.begin(), refined_ancestors.end(), key
	);
	for (const WtLodPlannerViewer &viewer : viewers) {
		std::uint32_t refinement_radius = viewer.refinement_radius_chunks != 0 ?
			viewer.refinement_radius_chunks :
			viewer.radius_chunks;
		if (viewer.refinement_radius_chunks == 0 &&
			refinement_radius_limit_chunks_ != 0) {
			refinement_radius = std::min(
				refinement_radius,
				refinement_radius_limit_chunks_
			);
		}
		if (refinement_radius == 0) continue;
		const double threshold_chunks =
			static_cast<double>(refinement_radius) +
			(was_refined ?
				static_cast<double>(kWtLodRefinementHysteresisChunks) :
				0.0);
		if (distance_to_chunk(viewer.snapshot, key) <
			child_extent * threshold_chunks) {
			return true;
		}
	}
	return false;
}

WtBalancedLodPlannerStatus WtBalancedLodPlanner::append_subtree(
	const WtChunkKey &key,
	const std::vector<WtLodPlannerViewer> &viewers,
	const std::vector<WtChunkKey> &refined_ancestors,
	const std::vector<WtChunkKey> &forced_leaf_keys,
	std::vector<WtChunkKey> &leaves,
	const std::function<bool()> &cancel_requested
) const {
	if (cancel_requested && cancel_requested()) {
		return WtBalancedLodPlannerStatus::Cancelled;
	}
	if (should_refine(key, viewers, refined_ancestors, forced_leaf_keys)) {
		std::array<WtChunkKey, 8> children{};
		if (page_hierarchy_.complete_children(key, children)) {
			for (const WtChunkKey &child : children) {
				const WtBalancedLodPlannerStatus status = append_subtree(
					child, viewers, refined_ancestors, forced_leaf_keys, leaves,
					cancel_requested
				);
				if (status != WtBalancedLodPlannerStatus::Ok) return status;
			}
			return WtBalancedLodPlannerStatus::Ok;
		}
	}
	if (leaves.size() >= active_capacity_) {
		return WtBalancedLodPlannerStatus::CapacityExceeded;
	}
	leaves.push_back(key);
	return WtBalancedLodPlannerStatus::Ok;
}

WtBalancedLodPlannerStatus WtBalancedLodPlanner::refine_leaf(
	std::vector<WtChunkKey> &leaves,
	std::size_t leaf_index
) const {
	if (leaf_index >= leaves.size() || leaves[leaf_index].lod == 0) {
		return WtBalancedLodPlannerStatus::InvalidLodMap;
	}
	if (leaves.size() > active_capacity_ - 7U) {
		return WtBalancedLodPlannerStatus::CapacityExceeded;
	}
	std::array<WtChunkKey, 8> children{};
	if (!page_hierarchy_.complete_children(leaves[leaf_index], children)) {
		return WtBalancedLodPlannerStatus::IncompleteHierarchy;
	}
	leaves.erase(leaves.begin() + static_cast<std::ptrdiff_t>(leaf_index));
	leaves.insert(leaves.end(), children.begin(), children.end());
	std::sort(leaves.begin(), leaves.end());
	return WtBalancedLodPlannerStatus::Ok;
}

WtBalancedLodPlannerStatus WtBalancedLodPlanner::balance(
	std::vector<WtChunkKey> &leaves,
	WtLodMap &lod_map,
	const std::function<bool()> &cancel_requested
) const {
	const std::size_t maximum_refinements =
		(active_capacity_ - leaves.size()) / 7U;
	for (std::size_t pass = 0; pass <= maximum_refinements; ++pass) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		const WtLodMapStatus status = lod_map.set_active_chunks(leaves);
		if (status == WtLodMapStatus::Ok) {
			return WtBalancedLodPlannerStatus::Ok;
		}
		if (status != WtLodMapStatus::LodDifferenceExceeded) {
			return WtBalancedLodPlannerStatus::InvalidLodMap;
		}
		const std::size_t coarse_index = find_unbalanced_coarse_leaf(leaves);
		if (coarse_index == leaves.size()) {
			return WtBalancedLodPlannerStatus::InvalidLodMap;
		}
		const WtBalancedLodPlannerStatus refine_status =
			refine_leaf(leaves, coarse_index);
		if (refine_status != WtBalancedLodPlannerStatus::Ok) {
			return refine_status;
		}
	}
	return WtBalancedLodPlannerStatus::InvalidLodMap;
}

WtBalancedLodPlannerStatus WtBalancedLodPlanner::plan(
	const std::vector<WtLodPlannerViewer> &viewers,
	const std::vector<WtDesiredChunk> &current_desired,
	const WtCollisionPolicy &collision_policy,
	WtBalancedLodPlan &output,
	bool visual_viewer_collision_enabled,
	const std::function<bool()> &cancel_requested,
	const std::vector<WtChunkKey> &forced_leaf_keys
) const {
	output.clear();
	if (!valid_ || !wt_is_valid_collision_policy(collision_policy)) {
		return WtBalancedLodPlannerStatus::InvalidConfiguration;
	}
	std::vector<WtLodPlannerViewer> ordered = viewers;
	std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
		return a.snapshot.id < b.snapshot.id;
	});
	std::uint8_t maximum_lod = 0;
	for (std::size_t index = 0; index < ordered.size(); ++index) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		const WtLodPlannerViewer &viewer = ordered[index];
		if (viewer.snapshot.id == 0 || viewer.snapshot.revision == 0 ||
			!std::isfinite(viewer.snapshot.x) ||
			!std::isfinite(viewer.snapshot.y) ||
			!std::isfinite(viewer.snapshot.z) ||
			viewer.maximum_lod > kWtMaximumLod ||
			!bounded_radius(viewer.radius_chunks) ||
			!bounded_radius(viewer.refinement_radius_chunks)) {
			return WtBalancedLodPlannerStatus::InvalidViewer;
		}
		if (index != 0 && ordered[index - 1].snapshot.id == viewer.snapshot.id) {
			return WtBalancedLodPlannerStatus::DuplicateViewer;
		}
		maximum_lod = std::max(maximum_lod, viewer.maximum_lod);
	}
	for (const WtChunkKey &key : forced_leaf_keys) {
		if (!wt_is_valid_chunk_key(key) || key.lod != 0 ||
				!catalog_contains(key)) {
			return WtBalancedLodPlannerStatus::InvalidLodMap;
		}
	}
	if (ordered.empty()) return WtBalancedLodPlannerStatus::Ok;
	std::vector<WtChunkKey> refined_ancestors;
	refined_ancestors.reserve(
		current_desired.size() * static_cast<std::size_t>(maximum_lod)
	);
	for (const WtDesiredChunk &desired : current_desired) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		if (!wt_is_valid_chunk_key(desired.key)) {
			continue;
		}
		WtChunkKey ancestor = desired.key;
		while (ancestor.lod < maximum_lod) {
			ancestor = wt_parent_chunk_key(ancestor);
			refined_ancestors.push_back(ancestor);
		}
	}
	std::sort(refined_ancestors.begin(), refined_ancestors.end());
	refined_ancestors.erase(
		std::unique(refined_ancestors.begin(), refined_ancestors.end()),
		refined_ancestors.end()
	);

	std::vector<WtChunkKey> roots;
	if (global_coarse_lod_coverage_) {
		const std::size_t coarse_root_count =
			page_hierarchy_.lod_page_count(maximum_lod);
		if (coarse_root_count <= active_capacity_) {
			roots.reserve(coarse_root_count);
			if (!page_hierarchy_.append_lod_keys(
					maximum_lod, roots, active_capacity_
				)) {
				return WtBalancedLodPlannerStatus::InvalidConfiguration;
			}
		}
	}
	for (const WtLodPlannerViewer &viewer : ordered) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		std::int32_t center_x = 0;
		std::int32_t center_y = 0;
		std::int32_t center_z = 0;
		if (!chunk_coordinate(viewer.snapshot.x, maximum_lod, center_x) ||
			!chunk_coordinate(viewer.snapshot.y, maximum_lod, center_y) ||
			!chunk_coordinate(viewer.snapshot.z, maximum_lod, center_z)) {
			return WtBalancedLodPlannerStatus::InvalidViewer;
		}
		std::vector<WtChunkKey> viewer_roots;
		if (!page_hierarchy_.query_viewer_roots(
				{ center_x, center_y, center_z, maximum_lod },
				viewer.radius_chunks,
				viewer_roots,
				active_capacity_
			)) {
			return WtBalancedLodPlannerStatus::CapacityExceeded;
		}
		roots.insert(roots.end(), viewer_roots.begin(), viewer_roots.end());
	}
	for (WtChunkKey key : forced_leaf_keys) {
		while (key.lod < maximum_lod) key = wt_parent_chunk_key(key);
		roots.push_back(key);
	}
	std::sort(roots.begin(), roots.end());
	roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
	std::vector<WtChunkKey> leaves;
	leaves.reserve(active_capacity_);
	for (const WtChunkKey &root : roots) {
		const WtBalancedLodPlannerStatus append_status = append_subtree(
			root, ordered, refined_ancestors, forced_leaf_keys, leaves,
			cancel_requested
		);
		if (append_status != WtBalancedLodPlannerStatus::Ok) return append_status;
	}
	std::sort(leaves.begin(), leaves.end());
	WtLodMap lod_map(active_capacity_);
	const WtBalancedLodPlannerStatus balance_status =
		balance(leaves, lod_map, cancel_requested);
	if (balance_status != WtBalancedLodPlannerStatus::Ok) return balance_status;
	output.entries = lod_map.get_entries();
	output.demands.reserve(output.entries.size());
	for (const WtLodMapEntry &entry : output.entries) {
		if (cancel_requested && cancel_requested()) {
			output.clear();
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		double nearest = std::numeric_limits<double>::infinity();
		bool collision_required = false;
		const WtDesiredChunk *current = find_current(current_desired, entry.key);
		for (const WtLodPlannerViewer &viewer : ordered) {
			const double distance = distance_to_chunk(viewer.snapshot, entry.key);
			nearest = std::min(nearest, distance);
			if (visual_viewer_collision_enabled) {
				const WtCollisionRequirement collision =
					wt_evaluate_collision_requirement(
						collision_policy,
						current != nullptr && current->collision_required,
						distance
					);
				if (collision == WtCollisionRequirement::Invalid) {
					output.clear();
					return WtBalancedLodPlannerStatus::InvalidConfiguration;
				}
				collision_required = collision_required ||
					collision == WtCollisionRequirement::Required;
			}
		}
		const double bounded = std::min(
			nearest,
			static_cast<double>(kWtDistancePriorityMaximum)
		);
		const std::int32_t lod_priority =
			static_cast<std::int32_t>(entry.key.lod) * kWtLodPriorityBand;
		const std::int32_t distance_priority =
			kWtDistancePriorityMaximum - static_cast<std::int32_t>(bounded);
		const std::int32_t priority = collision_required ?
			kWtCollisionPriorityMaximum - static_cast<std::int32_t>(std::min(
				nearest,
				static_cast<double>(kWtCollisionPriorityDistanceLimit)
			)) :
			lod_priority + distance_priority;
		output.demands.push_back({
			entry.key,
			priority,
			collision_required,
		});
	}
	return WtBalancedLodPlannerStatus::Ok;
}

WtBalancedLodPlannerStatus WtBalancedLodPlanner::stage_toward(
	const WtBalancedLodPlan &target,
	const WtBalancedLodPlan &current,
	const std::vector<WtChunkKey> &visually_ready,
	std::uint8_t staging_root_lod,
	std::size_t maximum_topology_changes,
	WtBalancedLodPlan &output,
	bool &complete,
	const std::vector<WtChunkKey> &preferred_refinement_keys,
	bool preferred_refinement_only,
	bool allow_unready_preferred_refinement,
	bool allow_preferred_coarsening,
	const std::function<bool()> &cancel_requested
) const {
	output.clear();
	complete = false;
	if (!valid_ || staging_root_lod > kWtMaximumLod ||
		maximum_topology_changes == 0 ||
		target.entries.size() != target.demands.size() ||
		current.entries.size() != current.demands.size()) {
		return WtBalancedLodPlannerStatus::InvalidConfiguration;
	}
	if (target.entries.empty()) {
		output = target;
		complete = true;
		return WtBalancedLodPlannerStatus::Ok;
	}
	std::vector<WtChunkKey> ready = visually_ready;
	std::sort(ready.begin(), ready.end());
	ready.erase(std::unique(ready.begin(), ready.end()), ready.end());
	for (const WtChunkKey &key : ready) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		if (!wt_is_valid_chunk_key(key)) {
			return WtBalancedLodPlannerStatus::InvalidLodMap;
		}
	}
	std::vector<WtChunkKey> preferred = preferred_refinement_keys;
	std::sort(preferred.begin(), preferred.end());
	preferred.erase(std::unique(preferred.begin(), preferred.end()), preferred.end());
	for (const WtChunkKey &key : preferred) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		if (!wt_is_valid_chunk_key(key)) {
			return WtBalancedLodPlannerStatus::InvalidLodMap;
		}
	}

	for (const WtLodMapEntry &entry : target.entries) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		if (!wt_is_valid_chunk_key(entry.key) ||
			!catalog_contains(entry.key) || entry.key.lod > staging_root_lod) {
			return WtBalancedLodPlannerStatus::InvalidLodMap;
		}
	}
	std::vector<WtChunkKey> target_roots;
	for (const WtLodMapEntry &entry : target.entries) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		WtChunkKey root = entry.key;
		while (root.lod < staging_root_lod) root = wt_parent_chunk_key(root);
		target_roots.push_back(root);
	}
	std::sort(target_roots.begin(), target_roots.end());
	target_roots.erase(
		std::unique(target_roots.begin(), target_roots.end()),
		target_roots.end()
	);

	std::vector<WtChunkKey> leaves;
	leaves.reserve(std::min(active_capacity_,
		current.entries.size() + target_roots.size()));
	for (const WtLodMapEntry &entry : current.entries) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		if (!wt_is_valid_chunk_key(entry.key) ||
			!catalog_contains(entry.key)) {
			return WtBalancedLodPlannerStatus::InvalidLodMap;
		}
		leaves.push_back(entry.key);
	}
	for (const WtChunkKey &root : target_roots) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		const bool covered = std::any_of(
			leaves.begin(), leaves.end(),
			[&](const WtChunkKey &leaf) { return keys_overlap(root, leaf); }
		);
		if (!covered) leaves.push_back(root);
	}
	std::sort(leaves.begin(), leaves.end());
	if (leaves.size() > active_capacity_) {
		return WtBalancedLodPlannerStatus::CapacityExceeded;
	}

	bool target_coverage_ready = true;
	for (const WtChunkKey &root : target_roots) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		bool covered = false;
		for (const WtChunkKey &leaf : leaves) {
			if (!bounds_contain(root, leaf)) continue;
			covered = true;
			if (!std::binary_search(ready.begin(), ready.end(), leaf)) {
				target_coverage_ready = false;
			}
		}
		if (!covered) target_coverage_ready = false;
	}
	if (target_coverage_ready) {
		leaves.erase(std::remove_if(
			leaves.begin(), leaves.end(),
			[&](const WtChunkKey &leaf) {
				return std::none_of(
					target_roots.begin(), target_roots.end(),
					[&](const WtChunkKey &root) {
						return bounds_contain(root, leaf);
					}
				);
			}
		), leaves.end());
	}
	const std::vector<TargetDescendantSummary> target_descendants =
		summarize_target_descendants(target.demands);

	std::size_t topology_changes = 0;
	while (topology_changes < maximum_topology_changes) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		bool changed = false;
		// Collapse one complete ready sibling family when the target contains
		// its parent. This is the inverse of refinement and keeps relocation
		// coarsening bounded by the same publication unit.
		for (const WtChunkKey &leaf : leaves) {
			if (cancel_requested && cancel_requested()) {
				return WtBalancedLodPlannerStatus::Cancelled;
			}
			// An edit-only batch must not spend its refinement budget on
			// unrelated background coarsening or enlarge its publication set.
			if (preferred_refinement_only && !allow_preferred_coarsening) break;
			if (leaf.lod >= kWtMaximumLod) continue;
			const WtChunkKey parent = wt_parent_chunk_key(leaf);
			const bool target_contains_parent =
				plan_covers_key(target.entries, parent);
			if (!target_contains_parent) continue;
			std::array<WtChunkKey, 8> children{};
			if (!page_hierarchy_.complete_children(parent, children)) {
				return WtBalancedLodPlannerStatus::IncompleteHierarchy;
			}
			const bool complete_family = std::all_of(
				children.begin(), children.end(),
				[&](const WtChunkKey &child) {
					return std::binary_search(leaves.begin(), leaves.end(), child) &&
						std::binary_search(ready.begin(), ready.end(), child);
				}
			);
			if (!complete_family) continue;
			for (const WtChunkKey &child : children) {
				leaves.erase(std::lower_bound(leaves.begin(), leaves.end(), child));
			}
			leaves.insert(
				std::lower_bound(leaves.begin(), leaves.end(), parent), parent
			);
			changed = true;
			break;
		}
		if (changed) {
			++topology_changes;
			continue;
		}

		WtLodMap current_map(active_capacity_);
		// A preferred split can require adjacent support families. Restore
		// 2:1 balance before selecting the next split in a direct refinement
		// batch; otherwise a valid intermediate step is rejected before the
		// final balance pass can run. No intermediate map is published.
		const WtBalancedLodPlannerStatus intermediate_balance =
			balance(leaves, current_map, cancel_requested);
		if (intermediate_balance != WtBalancedLodPlannerStatus::Ok) {
			return intermediate_balance;
		}
		const std::vector<WtLodMapEntry> current_entries =
			current_map.get_entries();
		const WtChunkKey *selected = nullptr;
		bool selected_preferred = false;
		std::uint8_t selected_target_lod = kWtMaximumLod + 1U;
		std::int32_t selected_priority = std::numeric_limits<std::int32_t>::min();
		for (const WtLodMapEntry &entry : current_entries) {
			if (cancel_requested && cancel_requested()) {
				return WtBalancedLodPlannerStatus::Cancelled;
			}
			const WtChunkKey &leaf = entry.key;
			const bool preferred_refinement = std::any_of(
				preferred.begin(), preferred.end(),
				[&](const WtChunkKey &key) {
					return bounds_contain(leaf, key);
				}
			);
			const bool visually_ready_leaf =
				std::binary_search(ready.begin(), ready.end(), leaf);
			if (leaf.lod == 0 || (!visually_ready_leaf &&
					!(allow_unready_preferred_refinement &&
						preferred_refinement))) {
				continue;
			}
			const bool borders_coarser =
				borders_coarser_leaf(leaf, current_entries);
			// Refining an edit-local leaf may require adjacent coarser leaves to
			// refine in the same balanced output. The balance pass supplies only
			// those mandatory 2:1 support families.
			if (borders_coarser && !preferred_refinement) continue;
			const TargetDescendantSummary *target_summary =
				find_target_descendants(target_descendants, leaf);
			if (target_summary == nullptr) continue;
			if (preferred_refinement_only && !preferred_refinement) continue;
			const std::int32_t priority = preferred_refinement ?
				target_summary->deepest_priority :
				target_summary->background_priority;
			std::array<WtChunkKey, 8> children{};
			if (!page_hierarchy_.complete_children(leaf, children)) {
				return WtBalancedLodPlannerStatus::IncompleteHierarchy;
			}
			const bool priority_precedes = selected == nullptr ||
				priority > selected_priority ||
				(priority == selected_priority && leaf < *selected);
			const bool preferred_precedes =
				target_summary->deepest_lod < selected_target_lod ||
				(target_summary->deepest_lod == selected_target_lod &&
					priority_precedes);
			const bool background_precedes = selected == nullptr ||
				leaf.lod > selected->lod ||
				(leaf.lod == selected->lod && priority_precedes);
			const bool select = selected == nullptr ||
				(preferred_refinement && !selected_preferred) ||
				(preferred_refinement == selected_preferred &&
					(preferred_refinement ? preferred_precedes :
						background_precedes));
			if (select) {
				selected = &entry.key;
				selected_preferred = preferred_refinement;
				selected_target_lod = target_summary->deepest_lod;
				selected_priority = priority;
			}
		}
		if (selected == nullptr) break;
		const auto leaf_iterator = std::lower_bound(
			leaves.begin(), leaves.end(), *selected
		);
		if (leaf_iterator == leaves.end() || *leaf_iterator != *selected) {
			return WtBalancedLodPlannerStatus::InvalidLodMap;
		}
		const std::size_t leaf_index = static_cast<std::size_t>(
			std::distance(leaves.begin(), leaf_iterator)
		);
		const WtBalancedLodPlannerStatus refine_status =
			refine_leaf(leaves, leaf_index);
		if (refine_status != WtBalancedLodPlannerStatus::Ok) {
			return refine_status;
		}
		++topology_changes;
	}

	WtLodMap staged_map(active_capacity_);
	const WtBalancedLodPlannerStatus balance_status =
		balance(leaves, staged_map, cancel_requested);
	if (balance_status != WtBalancedLodPlannerStatus::Ok) {
		return balance_status;
	}
	output.entries = staged_map.get_entries();
	if (same_lod_plan(output, target)) {
		output = target;
		complete = true;
		return WtBalancedLodPlannerStatus::Ok;
	}
	output.demands.reserve(output.entries.size());
	for (const WtLodMapEntry &entry : output.entries) {
		if (cancel_requested && cancel_requested()) {
			output.clear();
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		if (!append_staged_demand(
				entry.key, target, current, output.demands
			)) {
			output.clear();
			return WtBalancedLodPlannerStatus::InvalidLodMap;
		}
	}
	return WtBalancedLodPlannerStatus::Ok;
}

std::size_t WtBalancedLodPlanner::active_capacity() const noexcept {
	return active_capacity_;
}

WtBalancedLodPlannerStatus WtBalancedLodPlanner::stage_foreground(
	const WtBalancedLodPlan &target, const WtBalancedLodPlan &current,
	const std::vector<WtChunkKey> &visually_ready, std::uint8_t staging_root_lod,
	const std::vector<WtChunkKey> &foreground_keys,
	WtBalancedLodPlan &output, bool &complete,
	const std::function<bool()> &cancel_requested
) const {
	// Establish coverage and admit at most one obsolete family for coarsening.
	// Foreground refinement itself is a tree projection, not a sequence of
	// publish/wait/refine cycles. Only the final balanced map is requested.
	output.clear();
	complete = false;
	for (const auto &key : foreground_keys) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		if (!wt_is_valid_chunk_key(key)) return WtBalancedLodPlannerStatus::InvalidLodMap;
	}
	WtBalancedLodPlan base;
	const auto status = stage_toward(target, current, visually_ready, staging_root_lod,
		1, base, complete, {}, true, false, true, cancel_requested);
	if (status != WtBalancedLodPlannerStatus::Ok) return status;
	std::vector<WtChunkKey> refined_ancestors;
	for (const auto &entry : target.entries) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		for (auto ancestor = entry.key; ancestor.lod < staging_root_lod;) {
			ancestor = wt_parent_chunk_key(ancestor);
			refined_ancestors.push_back(ancestor);
		}
	}
	std::sort(refined_ancestors.begin(), refined_ancestors.end());
	refined_ancestors.erase(std::unique(refined_ancestors.begin(), refined_ancestors.end()), refined_ancestors.end());
	std::vector<WtChunkKey> work, leaves;
	for (const auto &entry : base.entries) work.push_back(entry.key);
	for (std::size_t cursor = 0; cursor < work.size(); ++cursor) {
		if (cancel_requested && cancel_requested()) {
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		const auto key = work[cursor];
		const bool foreground = std::any_of(foreground_keys.begin(), foreground_keys.end(),
			[&](const WtChunkKey &focus) { return bounds_contain(key, focus); });
		if (foreground && key.lod > 0 && std::binary_search(refined_ancestors.begin(), refined_ancestors.end(), key)) {
			std::array<WtChunkKey, 8> children{};
			if (!page_hierarchy_.complete_children(key, children)) return WtBalancedLodPlannerStatus::IncompleteHierarchy;
			// Pending leaves plus completed leaves are the actual output bound.
			if (leaves.size() + work.size() - cursor - 1 + children.size() > active_capacity_) {
				return WtBalancedLodPlannerStatus::CapacityExceeded;
			}
			work.insert(work.end(), children.begin(), children.end());
		} else {
			leaves.push_back(key);
		}
	}
	std::sort(leaves.begin(), leaves.end());
	WtLodMap map(active_capacity_);
	const auto balanced = balance(leaves, map, cancel_requested);
	if (balanced != WtBalancedLodPlannerStatus::Ok) return balanced;
	output.clear();
	output.entries = map.get_entries();
	complete = same_lod_plan(output, target);
	if (complete) {
		output = target;
		return WtBalancedLodPlannerStatus::Ok;
	}
	for (const auto &entry : output.entries) {
		if (cancel_requested && cancel_requested()) {
			output.clear();
			return WtBalancedLodPlannerStatus::Cancelled;
		}
		if (!append_staged_demand(entry.key, target, current, output.demands)) {
			output.clear();
			return WtBalancedLodPlannerStatus::InvalidLodMap;
		}
	}
	return WtBalancedLodPlannerStatus::Ok;
}

std::size_t WtBalancedLodPlanner::catalog_size() const noexcept {
	return page_hierarchy_.page_count();
}

WtPageHierarchyMetrics WtBalancedLodPlanner::hierarchy_metrics() const noexcept {
	return page_hierarchy_.metrics();
}

const char *wt_balanced_lod_planner_status_message(
	WtBalancedLodPlannerStatus status
) noexcept {
	switch (status) {
		case WtBalancedLodPlannerStatus::Ok: return "ok";
		case WtBalancedLodPlannerStatus::InvalidConfiguration:
			return "balanced LOD planner configuration is invalid";
		case WtBalancedLodPlannerStatus::InvalidViewer:
			return "balanced LOD viewer is invalid";
		case WtBalancedLodPlannerStatus::DuplicateViewer:
			return "balanced LOD viewer ID is duplicated";
		case WtBalancedLodPlannerStatus::CapacityExceeded:
			return "balanced LOD active capacity is exceeded";
		case WtBalancedLodPlannerStatus::IncompleteHierarchy:
			return "balanced LOD page hierarchy is incomplete";
		case WtBalancedLodPlannerStatus::InvalidLodMap:
			return "balanced LOD map is invalid";
		case WtBalancedLodPlannerStatus::Cancelled:
			return "balanced LOD planning was cancelled";
	}
	return "unknown balanced LOD planner status";
}

} // namespace world_transvoxel
