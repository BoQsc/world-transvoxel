#include "services/wt_page_meshing_runtime.h"

#include <algorithm>

namespace world_transvoxel {

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::cancel_generation(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	auto record = find_record(key);
	if (record == records_.end()) {
		return WtPageMeshingRuntimeStatus::NotFound;
	}
	if (record->generation != generation) {
		return WtPageMeshingRuntimeStatus::StaleCompletion;
	}
	if (record->mesh) {
		++metrics_.discarded_mesh_completions;
	}
	records_.erase(record);
	++metrics_.cancellations;
	return WtPageMeshingRuntimeStatus::Ok;
}

WtPageMeshingRuntimeStatus WtPageMeshingRuntimeService::release_chunk(
	const WtChunkKey &key
) {
	auto record = find_record(key);
	if (record == records_.end()) {
		return WtPageMeshingRuntimeStatus::NotFound;
	}
	if (record->mesh) {
		++metrics_.discarded_mesh_completions;
	}
	records_.erase(record);
	++metrics_.cancellations;
	return WtPageMeshingRuntimeStatus::Ok;
}

WtPageMeshingRuntimeStatus WtPageMeshingRuntimeService::reprioritize(
	const WtChunkKey &key,
	WtGenerationToken generation,
	std::int32_t priority
) {
	auto record = find_record(key);
	if (record == records_.end()) {
		return WtPageMeshingRuntimeStatus::NotFound;
	}
	if (record->generation != generation) {
		return WtPageMeshingRuntimeStatus::StaleCompletion;
	}
	record->priority = priority;
	return WtPageMeshingRuntimeStatus::Ok;
}

WtPageMeshingRuntimeStatus
WtPageMeshingRuntimeService::invalidate_dependency(
	const WtChunkKey &dependency,
	std::vector<WtPageMeshingInvalidation> &invalidated
) {
	invalidated.clear();
	if (!wt_is_valid_chunk_key(dependency)) {
		return WtPageMeshingRuntimeStatus::InvalidJob;
	}
	for (auto record = records_.begin(); record != records_.end();) {
		const auto found = std::lower_bound(
			record->dependencies.begin(),
			record->dependencies.end(),
			dependency,
			[](const Dependency &left, const WtChunkKey &right) {
				return left.key < right;
			}
		);
		const bool depends = found != record->dependencies.end() &&
			found->key == dependency;
		if (!depends) {
			++record;
			continue;
		}
		invalidated.push_back({ record->key, record->generation });
		if (record->mesh) {
			++metrics_.discarded_mesh_completions;
		}
		record = records_.erase(record);
		++metrics_.invalidated_records;
	}
	return invalidated.empty() ?
		WtPageMeshingRuntimeStatus::NotFound :
		WtPageMeshingRuntimeStatus::Ok;
}

WtPageMeshingRuntimeOwnerStatus
WtPageMeshingRuntimeService::cancel_owned_generation(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	const WtPageMeshingRuntimeStatus status =
		cancel_generation(key, generation);
	if (status == WtPageMeshingRuntimeStatus::Ok) {
		return WtPageMeshingRuntimeOwnerStatus::Ok;
	}
	return status == WtPageMeshingRuntimeStatus::NotFound ?
		WtPageMeshingRuntimeOwnerStatus::NotFound :
		WtPageMeshingRuntimeOwnerStatus::StaleGeneration;
}

WtPageMeshingRuntimeOwnerStatus
WtPageMeshingRuntimeService::release_owned_chunk(const WtChunkKey &key) {
	return release_chunk(key) == WtPageMeshingRuntimeStatus::Ok ?
		WtPageMeshingRuntimeOwnerStatus::Ok :
		WtPageMeshingRuntimeOwnerStatus::NotFound;
}

WtPageMeshingRuntimeOwnerStatus
WtPageMeshingRuntimeService::reprioritize_owned_chunk(
	const WtChunkKey &key,
	WtGenerationToken generation,
	std::int32_t priority
) {
	const WtPageMeshingRuntimeStatus status =
		reprioritize(key, generation, priority);
	if (status == WtPageMeshingRuntimeStatus::Ok) {
		return WtPageMeshingRuntimeOwnerStatus::Ok;
	}
	return status == WtPageMeshingRuntimeStatus::NotFound ?
		WtPageMeshingRuntimeOwnerStatus::NotFound :
		WtPageMeshingRuntimeOwnerStatus::StaleGeneration;
}

std::vector<WtPageMeshingRuntimeRecordSnapshot>
WtPageMeshingRuntimeService::get_records() const {
	std::vector<WtPageMeshingRuntimeRecordSnapshot> snapshots;
	snapshots.reserve(records_.size());
	for (const Record &record : records_) {
		std::size_t pins = 0;
		for (const Dependency &dependency : record.dependencies) {
			pins += dependency.page ? 1U : 0U;
		}
		snapshots.push_back({
			record.key,
			record.generation,
			record.source_revision,
			record.world_revision,
			record.priority,
			record.transition_mask,
			record.phase,
			record.dependencies.size(),
			pins,
		});
	}
	return snapshots;
}

std::size_t WtPageMeshingRuntimeService::record_count() const noexcept {
	return records_.size();
}

std::size_t WtPageMeshingRuntimeService::record_capacity() const noexcept {
	return record_capacity_;
}

std::size_t WtPageMeshingRuntimeService::pinned_page_count() const noexcept {
	std::size_t count = 0;
	for (const Record &record : records_) {
		for (const Dependency &dependency : record.dependencies) {
			count += dependency.page ? 1U : 0U;
		}
	}
	return count;
}

WtPageMeshingRuntimeMetrics
WtPageMeshingRuntimeService::get_metrics() const noexcept {
	return metrics_;
}

} // namespace world_transvoxel
