#include "streaming/wt_foreground_priority.h"

#include <algorithm>

namespace world_transvoxel {
namespace {

bool valid_priority_class(WtForegroundPriorityClass value) noexcept {
	return value == WtForegroundPriorityClass::PlayerSupport ||
		value == WtForegroundPriorityClass::InteractionFocus;
}

bool canonical_keys(const std::vector<WtChunkKey> &keys) noexcept {
	if (keys.size() > kWtForegroundPriorityKeysPerSource) return false;
	for (std::size_t index = 0; index < keys.size(); ++index) {
		if (!wt_is_valid_chunk_key(keys[index]) ||
			(index != 0 && !(keys[index - 1] < keys[index]))) {
			return false;
		}
	}
	return true;
}

} // namespace

std::int32_t wt_foreground_priority(
	WtForegroundPriorityClass priority_class
) noexcept {
	return priority_class == WtForegroundPriorityClass::PlayerSupport ?
		kWtPlayerSupportPriority : kWtInteractionFocusPriority;
}

WtForegroundPriorityStatus WtForegroundPriorityLeaseSet::update(
	const WtForegroundPriorityLeaseRequest &request
) {
	if (request.source_id == 0 || request.revision == 0 ||
		!valid_priority_class(request.priority_class) ||
		!canonical_keys(request.keys)) {
		return WtForegroundPriorityStatus::InvalidRequest;
	}
	const auto source = std::lower_bound(
		sources_.begin(),
		sources_.end(),
		request.source_id,
		[](const SourceRecord &record, std::uint64_t source_id) {
			return record.source_id < source_id;
		}
	);
	if (source != sources_.end() && source->source_id == request.source_id) {
		if (request.revision <= source->revision) {
			return WtForegroundPriorityStatus::StaleRevision;
		}
		source->revision = request.revision;
		source->priority_class = request.priority_class;
		source->keys = request.keys;
		return WtForegroundPriorityStatus::Ok;
	}
	if (sources_.size() >= kWtForegroundPrioritySourceCapacity) {
		return WtForegroundPriorityStatus::SourceCapacityExceeded;
	}
	sources_.insert(source, {
		request.source_id,
		request.revision,
		request.priority_class,
		request.keys,
	});
	return WtForegroundPriorityStatus::Ok;
}

WtForegroundPriorityOverlayResult WtForegroundPriorityLeaseSet::apply(
	const std::vector<WtViewerChunkDemand> &base,
	std::vector<WtViewerChunkDemand> &effective
) const {
	effective = base;
	WtForegroundPriorityOverlayResult result;
	for (const SourceRecord &source : sources_) {
		const std::int32_t priority = wt_foreground_priority(
			source.priority_class
		);
		for (const WtChunkKey &key : source.keys) {
			++result.requested_keys;
			const auto demand = std::lower_bound(
				effective.begin(),
				effective.end(),
				key,
				[](const WtViewerChunkDemand &item, const WtChunkKey &value) {
					return item.key < value;
				}
			);
			if (demand == effective.end() || demand->key != key) {
				++result.missing_keys;
				continue;
			}
			++result.matched_keys;
			if (demand->priority < priority) {
				demand->priority = priority;
				++result.changed_priorities;
			}
		}
	}
	return result;
}

std::size_t WtForegroundPriorityLeaseSet::source_count() const noexcept {
	return sources_.size();
}

std::size_t
WtForegroundPriorityLeaseSet::active_source_count() const noexcept {
	return static_cast<std::size_t>(std::count_if(
		sources_.begin(),
		sources_.end(),
		[](const SourceRecord &source) { return !source.keys.empty(); }
	));
}

std::size_t WtForegroundPriorityLeaseSet::active_key_count(
	WtForegroundPriorityClass priority_class
) const noexcept {
	std::size_t count = 0;
	for (const SourceRecord &source : sources_) {
		if (source.priority_class == priority_class) {
			count += source.keys.size();
		}
	}
	return count;
}

} // namespace world_transvoxel
