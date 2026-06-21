#include "streaming/wt_multi_viewer_desired_set.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace world_transvoxel {
namespace {

bool demand_less(
	const WtViewerChunkDemand &left,
	const WtViewerChunkDemand &right
) noexcept {
	return left.key < right.key;
}

bool valid_snapshot(const WtViewerSnapshot &snapshot) noexcept {
	return snapshot.id != 0 &&
		snapshot.revision != 0 &&
		std::isfinite(snapshot.x) &&
		std::isfinite(snapshot.y) &&
		std::isfinite(snapshot.z);
}

} // namespace

bool WtDesiredChunk::operator==(const WtDesiredChunk &other) const noexcept {
	return key == other.key &&
		priority == other.priority &&
		supporter_count == other.supporter_count &&
		collision_required == other.collision_required;
}

void WtDesiredSetDelta::clear() noexcept {
	added.clear();
	removed.clear();
	updated.clear();
}

WtMultiViewerDesiredSet::WtMultiViewerDesiredSet(
	WtMultiViewerDesiredSetLimits limits
) : limits_(limits) {
	valid_ =
		limits_.viewer_capacity != 0 &&
		limits_.viewer_capacity <= kWtMaximumDesiredViewerCount &&
		limits_.demand_capacity_per_viewer != 0 &&
		limits_.demand_capacity_per_viewer <= kWtMaximumDesiredChunkCount &&
		limits_.total_demand_capacity != 0 &&
		limits_.total_demand_capacity <= kWtMaximumDesiredChunkCount &&
		limits_.desired_chunk_capacity != 0 &&
		limits_.desired_chunk_capacity <= kWtMaximumDesiredChunkCount &&
		limits_.demand_capacity_per_viewer <= limits_.total_demand_capacity;
	if (valid_) {
		viewers_.reserve(limits_.viewer_capacity);
		desired_.reserve(limits_.desired_chunk_capacity);
	}
}

bool WtMultiViewerDesiredSet::valid() const noexcept {
	return valid_;
}

WtMultiViewerDesiredSetStatus WtMultiViewerDesiredSet::validate_demands(
	const std::vector<WtViewerChunkDemand> &demands,
	std::vector<WtViewerChunkDemand> &ordered
) const {
	ordered.clear();
	if (demands.size() > limits_.demand_capacity_per_viewer) {
		return WtMultiViewerDesiredSetStatus::ViewerDemandCapacityExceeded;
	}
	ordered = demands;
	std::sort(ordered.begin(), ordered.end(), demand_less);
	for (std::size_t index = 0; index < ordered.size(); ++index) {
		if (!wt_is_valid_chunk_key(ordered[index].key)) {
			ordered.clear();
			return WtMultiViewerDesiredSetStatus::InvalidDemand;
		}
		if (index != 0 && ordered[index - 1].key == ordered[index].key) {
			ordered.clear();
			return WtMultiViewerDesiredSetStatus::DuplicateDemand;
		}
	}
	return WtMultiViewerDesiredSetStatus::Ok;
}

WtMultiViewerDesiredSetStatus WtMultiViewerDesiredSet::build_union(
	const std::vector<ViewerRecord> &viewers,
	std::vector<WtDesiredChunk> &desired,
	std::uint64_t &processed_count
) const {
	desired.clear();
	processed_count = 0;
	std::vector<WtViewerChunkDemand> combined;
	combined.reserve(limits_.total_demand_capacity);
	for (const ViewerRecord &viewer : viewers) {
		if (viewer.demands.size() >
			limits_.total_demand_capacity - combined.size()) {
			return WtMultiViewerDesiredSetStatus::TotalDemandCapacityExceeded;
		}
		combined.insert(
			combined.end(),
			viewer.demands.begin(),
			viewer.demands.end()
		);
	}
	processed_count = combined.size();
	std::sort(combined.begin(), combined.end(), demand_less);
	for (std::size_t index = 0; index < combined.size();) {
		const WtChunkKey key = combined[index].key;
		std::int32_t priority = combined[index].priority;
		std::uint32_t supporters = 0;
		bool collision_required = false;
		do {
			priority = std::max(priority, combined[index].priority);
			collision_required =
				collision_required || combined[index].collision_required;
			if (supporters == std::numeric_limits<std::uint32_t>::max()) {
				return WtMultiViewerDesiredSetStatus::DesiredChunkCapacityExceeded;
			}
			++supporters;
			++index;
		} while (index < combined.size() && combined[index].key == key);
		if (desired.size() >= limits_.desired_chunk_capacity) {
			desired.clear();
			return WtMultiViewerDesiredSetStatus::DesiredChunkCapacityExceeded;
		}
		desired.push_back({ key, priority, supporters, collision_required });
	}
	return WtMultiViewerDesiredSetStatus::Ok;
}

void WtMultiViewerDesiredSet::build_delta(
	const std::vector<WtDesiredChunk> &candidate,
	WtDesiredSetDelta &delta
) const {
	delta.clear();
	delta.added.reserve(candidate.size());
	delta.removed.reserve(desired_.size());
	delta.updated.reserve(std::min(candidate.size(), desired_.size()));
	std::size_t current_index = 0;
	std::size_t candidate_index = 0;
	while (current_index < desired_.size() ||
		candidate_index < candidate.size()) {
		if (current_index == desired_.size()) {
			delta.added.push_back(candidate[candidate_index++]);
			continue;
		}
		if (candidate_index == candidate.size()) {
			delta.removed.push_back(desired_[current_index++].key);
			continue;
		}
		const WtDesiredChunk &current = desired_[current_index];
		const WtDesiredChunk &next = candidate[candidate_index];
		if (current.key < next.key) {
			delta.removed.push_back(current.key);
			++current_index;
		} else if (next.key < current.key) {
			delta.added.push_back(next);
			++candidate_index;
		} else {
			if (!(current == next)) {
				delta.updated.push_back(next);
			}
			++current_index;
			++candidate_index;
		}
	}
}

WtMultiViewerDesiredSetStatus WtMultiViewerDesiredSet::update_viewer(
	const WtViewerSnapshot &snapshot,
	const std::vector<WtViewerChunkDemand> &demands,
	WtDesiredSetDelta &delta
) {
	delta.clear();
	if (!valid_) {
		return WtMultiViewerDesiredSetStatus::InvalidConfiguration;
	}
	if (!valid_snapshot(snapshot)) {
		++metrics_.rejected_events;
		return WtMultiViewerDesiredSetStatus::InvalidViewer;
	}
	std::vector<WtViewerChunkDemand> ordered;
	WtMultiViewerDesiredSetStatus status = validate_demands(demands, ordered);
	if (status != WtMultiViewerDesiredSetStatus::Ok) {
		++metrics_.rejected_events;
		return status;
	}

	std::vector<ViewerRecord> candidate = viewers_;
	const auto viewer = std::lower_bound(
		candidate.begin(),
		candidate.end(),
		snapshot.id,
		[](const ViewerRecord &record, std::uint64_t id) {
			return record.snapshot.id < id;
		}
	);
	if (viewer != candidate.end() && viewer->snapshot.id == snapshot.id) {
		if (snapshot.revision <= viewer->snapshot.revision) {
			++metrics_.rejected_events;
			return WtMultiViewerDesiredSetStatus::StaleViewerRevision;
		}
		viewer->snapshot = snapshot;
		viewer->demands = std::move(ordered);
	} else {
		if (candidate.size() >= limits_.viewer_capacity) {
			++metrics_.rejected_events;
			return WtMultiViewerDesiredSetStatus::ViewerCapacityExceeded;
		}
		candidate.insert(viewer, { snapshot, std::move(ordered) });
	}

	std::vector<WtDesiredChunk> desired;
	desired.reserve(limits_.desired_chunk_capacity);
	std::uint64_t processed_count = 0;
	status = build_union(candidate, desired, processed_count);
	if (status != WtMultiViewerDesiredSetStatus::Ok) {
		++metrics_.rejected_events;
		return status;
	}
	build_delta(desired, delta);
	viewers_.swap(candidate);
	desired_.swap(desired);
	total_demand_count_ = static_cast<std::size_t>(processed_count);
	++metrics_.viewer_updates;
	++metrics_.union_rebuilds;
	metrics_.demand_items_processed += processed_count;
	metrics_.chunks_added += delta.added.size();
	metrics_.chunks_removed += delta.removed.size();
	metrics_.chunks_updated += delta.updated.size();
	return WtMultiViewerDesiredSetStatus::Ok;
}

WtMultiViewerDesiredSetStatus WtMultiViewerDesiredSet::remove_viewer(
	std::uint64_t viewer_id,
	std::uint64_t revision,
	WtDesiredSetDelta &delta
) {
	delta.clear();
	if (!valid_) {
		return WtMultiViewerDesiredSetStatus::InvalidConfiguration;
	}
	if (viewer_id == 0 || revision == 0) {
		++metrics_.rejected_events;
		return WtMultiViewerDesiredSetStatus::InvalidViewer;
	}
	const auto viewer = std::lower_bound(
		viewers_.begin(),
		viewers_.end(),
		viewer_id,
		[](const ViewerRecord &record, std::uint64_t id) {
			return record.snapshot.id < id;
		}
	);
	if (viewer == viewers_.end() || viewer->snapshot.id != viewer_id) {
		++metrics_.rejected_events;
		return WtMultiViewerDesiredSetStatus::ViewerNotFound;
	}
	if (revision <= viewer->snapshot.revision) {
		++metrics_.rejected_events;
		return WtMultiViewerDesiredSetStatus::StaleViewerRevision;
	}
	std::vector<ViewerRecord> candidate = viewers_;
	candidate.erase(candidate.begin() + (viewer - viewers_.begin()));
	std::vector<WtDesiredChunk> desired;
	desired.reserve(limits_.desired_chunk_capacity);
	std::uint64_t processed_count = 0;
	const WtMultiViewerDesiredSetStatus status =
		build_union(candidate, desired, processed_count);
	if (status != WtMultiViewerDesiredSetStatus::Ok) {
		++metrics_.rejected_events;
		return status;
	}
	build_delta(desired, delta);
	viewers_.swap(candidate);
	desired_.swap(desired);
	total_demand_count_ = static_cast<std::size_t>(processed_count);
	++metrics_.viewer_removals;
	++metrics_.union_rebuilds;
	metrics_.demand_items_processed += processed_count;
	metrics_.chunks_added += delta.added.size();
	metrics_.chunks_removed += delta.removed.size();
	metrics_.chunks_updated += delta.updated.size();
	return WtMultiViewerDesiredSetStatus::Ok;
}

const std::vector<WtDesiredChunk> &
WtMultiViewerDesiredSet::get_desired_chunks() const noexcept {
	return desired_;
}

const WtDesiredChunk *WtMultiViewerDesiredSet::find_desired(
	const WtChunkKey &key
) const noexcept {
	const auto entry = std::lower_bound(
		desired_.begin(),
		desired_.end(),
		key,
		[](const WtDesiredChunk &item, const WtChunkKey &value) {
			return item.key < value;
		}
	);
	return entry != desired_.end() && entry->key == key ? &*entry : nullptr;
}

std::size_t WtMultiViewerDesiredSet::viewer_count() const noexcept {
	return viewers_.size();
}

std::size_t WtMultiViewerDesiredSet::total_demand_count() const noexcept {
	return total_demand_count_;
}

WtMultiViewerDesiredSetMetrics
WtMultiViewerDesiredSet::get_metrics() const noexcept {
	return metrics_;
}

} // namespace world_transvoxel
