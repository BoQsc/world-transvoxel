#include "streaming/wt_balanced_lod_planner.h"

#include "services/wt_runtime_config.h"
#include "storage/wt_world_manifest.h"

#include <algorithm>
#include <array>
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

bool intervals_overlap(
	std::int64_t a_minimum,
	std::int64_t a_maximum,
	std::int64_t b_minimum,
	std::int64_t b_maximum
) noexcept {
	return a_minimum < b_maximum && b_minimum < a_maximum;
}

bool face_neighbor(
	const WtChunkBounds &a,
	const WtChunkBounds &b
) noexcept {
	return ((a.maximum.x == b.minimum.x || a.minimum.x == b.maximum.x) &&
		intervals_overlap(a.minimum.y, a.maximum.y, b.minimum.y, b.maximum.y) &&
		intervals_overlap(a.minimum.z, a.maximum.z, b.minimum.z, b.maximum.z)) ||
		((a.maximum.y == b.minimum.y || a.minimum.y == b.maximum.y) &&
		intervals_overlap(a.minimum.x, a.maximum.x, b.minimum.x, b.maximum.x) &&
		intervals_overlap(a.minimum.z, a.maximum.z, b.minimum.z, b.maximum.z)) ||
		((a.maximum.z == b.minimum.z || a.minimum.z == b.maximum.z) &&
		intervals_overlap(a.minimum.x, a.maximum.x, b.minimum.x, b.maximum.x) &&
		intervals_overlap(a.minimum.y, a.maximum.y, b.minimum.y, b.maximum.y));
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

std::array<WtChunkKey, 8> child_keys(const WtChunkKey &parent) noexcept {
	std::array<WtChunkKey, 8> children{};
	std::size_t index = 0;
	for (std::int32_t z = 0; z < 2; ++z) {
		for (std::int32_t y = 0; y < 2; ++y) {
			for (std::int32_t x = 0; x < 2; ++x) {
				children[index++] = {
					static_cast<std::int32_t>(parent.x * 2 + x),
					static_cast<std::int32_t>(parent.y * 2 + y),
					static_cast<std::int32_t>(parent.z * 2 + z),
					static_cast<std::uint8_t>(parent.lod - 1),
				};
			}
		}
	}
	return children;
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
) :
		active_capacity_(active_capacity),
		refinement_radius_limit_chunks_(refinement_radius_limit_chunks),
		global_coarse_lod_coverage_(global_coarse_lod_coverage),
		page_catalog_(std::move(page_catalog)) {
	std::sort(page_catalog_.begin(), page_catalog_.end());
	valid_ = active_capacity_ != 0 &&
		active_capacity_ <= kWtMaximumRuntimeActiveChunks &&
		page_catalog_.size() <= kWtMaximumWorldPageCount;
	for (std::size_t index = 0; valid_ && index < page_catalog_.size(); ++index) {
		const WtChunkKey key = page_catalog_[index];
		const bool refinable_coordinates = key.lod == 0 ||
			(key.x >= std::numeric_limits<std::int32_t>::min() / 2 &&
				key.x <= std::numeric_limits<std::int32_t>::max() / 2 &&
				key.y >= std::numeric_limits<std::int32_t>::min() / 2 &&
				key.y <= std::numeric_limits<std::int32_t>::max() / 2 &&
				key.z >= std::numeric_limits<std::int32_t>::min() / 2 &&
				key.z <= std::numeric_limits<std::int32_t>::max() / 2);
		valid_ = wt_is_valid_chunk_key(key) && refinable_coordinates &&
			(index == 0 || page_catalog_[index - 1] < page_catalog_[index]);
	}
}

bool WtBalancedLodPlanner::valid() const noexcept {
	return valid_;
}

bool WtBalancedLodPlanner::catalog_contains(
	const WtChunkKey &key
) const noexcept {
	return std::binary_search(page_catalog_.begin(), page_catalog_.end(), key);
}

bool WtBalancedLodPlanner::should_refine(
	const WtChunkKey &key,
	const std::vector<WtLodPlannerViewer> &viewers,
	const std::vector<WtChunkKey> &refined_ancestors
) const noexcept {
	if (key.lod == 0) return false;
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

bool WtBalancedLodPlanner::append_subtree(
	const WtChunkKey &key,
	const std::vector<WtLodPlannerViewer> &viewers,
	const std::vector<WtChunkKey> &refined_ancestors,
	std::vector<WtChunkKey> &leaves
) const {
	if (should_refine(key, viewers, refined_ancestors)) {
		const auto children = child_keys(key);
		const bool complete = std::all_of(
			children.begin(), children.end(),
			[&](const WtChunkKey &child) { return catalog_contains(child); }
		);
		if (complete) {
			for (const WtChunkKey &child : children) {
				if (!append_subtree(
						child, viewers, refined_ancestors, leaves
					)) return false;
			}
			return true;
		}
	}
	if (leaves.size() >= active_capacity_) return false;
	leaves.push_back(key);
	return true;
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
	const auto children = child_keys(leaves[leaf_index]);
	if (!std::all_of(
			children.begin(), children.end(),
			[&](const WtChunkKey &child) { return catalog_contains(child); }
		)) {
		return WtBalancedLodPlannerStatus::IncompleteHierarchy;
	}
	leaves.erase(leaves.begin() + static_cast<std::ptrdiff_t>(leaf_index));
	leaves.insert(leaves.end(), children.begin(), children.end());
	std::sort(leaves.begin(), leaves.end());
	return WtBalancedLodPlannerStatus::Ok;
}

WtBalancedLodPlannerStatus WtBalancedLodPlanner::balance(
	std::vector<WtChunkKey> &leaves,
	WtLodMap &lod_map
) const {
	const std::size_t maximum_refinements =
		(active_capacity_ - leaves.size()) / 7U;
	for (std::size_t pass = 0; pass <= maximum_refinements; ++pass) {
		const WtLodMapStatus status = lod_map.set_active_chunks(leaves);
		if (status == WtLodMapStatus::Ok) {
			return WtBalancedLodPlannerStatus::Ok;
		}
		if (status != WtLodMapStatus::LodDifferenceExceeded) {
			return WtBalancedLodPlannerStatus::InvalidLodMap;
		}
		bool refined = false;
		for (std::size_t a = 0; !refined && a < leaves.size(); ++a) {
			const WtChunkBounds a_bounds = wt_chunk_bounds(leaves[a]);
			for (std::size_t b = a + 1; b < leaves.size(); ++b) {
				if (!face_neighbor(a_bounds, wt_chunk_bounds(leaves[b]))) continue;
				const int difference = static_cast<int>(leaves[a].lod) -
					static_cast<int>(leaves[b].lod);
				if (difference > 1 || difference < -1) {
					const WtBalancedLodPlannerStatus refine_status = refine_leaf(
						leaves, difference > 1 ? a : b
					);
					if (refine_status != WtBalancedLodPlannerStatus::Ok) {
						return refine_status;
					}
					refined = true;
				}
				if (refined) break;
			}
		}
		if (!refined) return WtBalancedLodPlannerStatus::InvalidLodMap;
	}
	return WtBalancedLodPlannerStatus::InvalidLodMap;
}

WtBalancedLodPlannerStatus WtBalancedLodPlanner::plan(
	const std::vector<WtLodPlannerViewer> &viewers,
	const std::vector<WtDesiredChunk> &current_desired,
	const WtCollisionPolicy &collision_policy,
	WtBalancedLodPlan &output
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
	if (ordered.empty()) return WtBalancedLodPlannerStatus::Ok;
	std::vector<WtChunkKey> refined_ancestors;
	refined_ancestors.reserve(
		current_desired.size() * static_cast<std::size_t>(maximum_lod)
	);
	for (const WtDesiredChunk &desired : current_desired) {
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
		std::size_t coarse_root_count = 0;
		for (const WtChunkKey &key : page_catalog_) {
			if (key.lod == maximum_lod) {
				++coarse_root_count;
			}
		}
		if (coarse_root_count <= active_capacity_) {
			roots.reserve(coarse_root_count);
			for (const WtChunkKey &key : page_catalog_) {
				if (key.lod == maximum_lod) {
					roots.push_back(key);
				}
			}
		}
	}
	for (const WtLodPlannerViewer &viewer : ordered) {
		std::int32_t center_x = 0;
		std::int32_t center_y = 0;
		std::int32_t center_z = 0;
		if (!chunk_coordinate(viewer.snapshot.x, maximum_lod, center_x) ||
			!chunk_coordinate(viewer.snapshot.y, maximum_lod, center_y) ||
			!chunk_coordinate(viewer.snapshot.z, maximum_lod, center_z)) {
			return WtBalancedLodPlannerStatus::InvalidViewer;
		}
		const std::int64_t radius = viewer.radius_chunks;
		for (std::int64_t z = -radius; z <= radius; ++z) {
			for (std::int64_t y = -radius; y <= radius; ++y) {
				for (std::int64_t x = -radius; x <= radius; ++x) {
					const std::int64_t px = static_cast<std::int64_t>(center_x) + x;
					const std::int64_t py = static_cast<std::int64_t>(center_y) + y;
					const std::int64_t pz = static_cast<std::int64_t>(center_z) + z;
					if (px < std::numeric_limits<std::int32_t>::min() ||
						px > std::numeric_limits<std::int32_t>::max() ||
						py < std::numeric_limits<std::int32_t>::min() ||
						py > std::numeric_limits<std::int32_t>::max() ||
						pz < std::numeric_limits<std::int32_t>::min() ||
						pz > std::numeric_limits<std::int32_t>::max()) continue;
					const WtChunkKey root {
						static_cast<std::int32_t>(px),
						static_cast<std::int32_t>(py),
						static_cast<std::int32_t>(pz),
						maximum_lod,
					};
					if (catalog_contains(root)) roots.push_back(root);
				}
			}
		}
	}
	std::sort(roots.begin(), roots.end());
	roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
	std::vector<WtChunkKey> leaves;
	leaves.reserve(active_capacity_);
	for (const WtChunkKey &root : roots) {
		if (!append_subtree(root, ordered, refined_ancestors, leaves)) {
			return WtBalancedLodPlannerStatus::CapacityExceeded;
		}
	}
	std::sort(leaves.begin(), leaves.end());
	WtLodMap lod_map(active_capacity_);
	const WtBalancedLodPlannerStatus balance_status = balance(leaves, lod_map);
	if (balance_status != WtBalancedLodPlannerStatus::Ok) return balance_status;
	output.entries = lod_map.get_entries();
	output.demands.reserve(output.entries.size());
	for (const WtLodMapEntry &entry : output.entries) {
		double nearest = std::numeric_limits<double>::infinity();
		bool collision_required = false;
		const WtDesiredChunk *current = find_current(current_desired, entry.key);
		for (const WtLodPlannerViewer &viewer : ordered) {
			const double distance = distance_to_chunk(viewer.snapshot, entry.key);
			nearest = std::min(nearest, distance);
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

std::size_t WtBalancedLodPlanner::active_capacity() const noexcept {
	return active_capacity_;
}

std::size_t WtBalancedLodPlanner::catalog_size() const noexcept {
	return page_catalog_.size();
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
	}
	return "unknown balanced LOD planner status";
}

} // namespace world_transvoxel
