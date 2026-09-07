#include "services/wt_page_meshing_runtime.h"

#include "storage/wt_async_storage_service.h"

#include <algorithm>

namespace world_transvoxel {

void WtPageMeshingRuntimeService::cancel_orphaned_dependency_requests(
	const std::vector<Dependency> &removed_dependencies
) noexcept {
	if (storage_ == nullptr) return;
	for (const Dependency &removed : removed_dependencies) {
		if (!removed.request_pending) continue;
		const bool still_needed = std::any_of(
			records_.begin(), records_.end(),
			[&](const Record &record) {
				return std::any_of(
					record.dependencies.begin(), record.dependencies.end(),
					[&](const Dependency &dependency) {
						return dependency.key == removed.key &&
							dependency.request_pending;
					}
				);
			}
		);
		if (!still_needed && storage_->cancel_queued_page(
				removed.key,
				WtStorageRequestSource::PageMeshing
			)) {
			++metrics_.cancelled_dependency_requests;
		}
	}
}

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
	cancel_async_work(key, generation);
	if (record->mesh) {
		++metrics_.discarded_mesh_completions;
	}
	const std::vector<Dependency> removed_dependencies = record->dependencies;
	records_.erase(record);
	cancel_orphaned_dependency_requests(removed_dependencies);
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
	cancel_async_work(key, record->generation);
	const std::vector<Dependency> removed_dependencies = record->dependencies;
	records_.erase(record);
	cancel_orphaned_dependency_requests(removed_dependencies);
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
	for (WtGpuMeshingShadowCapture &capture : record->deferred_gpu_captures) {
		capture.job.priority = priority;
	}
	reprioritize_async_work(key, generation, priority);
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
		cancel_async_work(record->key, record->generation);
		if (record->mesh) {
			++metrics_.discarded_mesh_completions;
		}
		const std::vector<Dependency> removed_dependencies =
			record->dependencies;
		record = records_.erase(record);
		cancel_orphaned_dependency_requests(removed_dependencies);
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
			record.cached_transition_mask,
			record.phase,
			record.dependencies.size(),
			pins,
		});
	}
	return snapshots;
}

bool WtPageMeshingRuntimeService::copy_record(
	const WtChunkKey &key,
	WtGenerationToken generation,
	WtPageMeshingRuntimeRecordSnapshot &output
) const noexcept {
	const auto record = find_record(key);
	if (record == records_.end() || record->generation != generation) {
		return false;
	}
	std::size_t pins = 0;
	for (const Dependency &dependency : record->dependencies) {
		pins += dependency.page ? 1U : 0U;
	}
	output = {
		record->key,
		record->generation,
		record->source_revision,
		record->world_revision,
		record->priority,
		record->transition_mask,
		record->cached_transition_mask,
		record->phase,
		record->dependencies.size(),
		pins,
	};
	return true;
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
	WtPageMeshingRuntimeMetrics snapshot = metrics_;
	merge_async_metrics(snapshot);
	for (const Record &record : records_) {
		switch (record.phase) {
			case WtPageMeshingRuntimePhase::Loading:
				++snapshot.loading_records;
				break;
			case WtPageMeshingRuntimePhase::SampleReady:
			case WtPageMeshingRuntimePhase::SampleFailedReady:
				++snapshot.sample_ready_records;
				break;
			case WtPageMeshingRuntimePhase::AwaitingMesh:
				++snapshot.awaiting_mesh_records;
				break;
			case WtPageMeshingRuntimePhase::Meshing:
			case WtPageMeshingRuntimePhase::AwaitingGpuCapture:
				++snapshot.meshing_records;
				break;
			case WtPageMeshingRuntimePhase::MeshReady:
			case WtPageMeshingRuntimePhase::MeshFailedReady:
				++snapshot.mesh_ready_records;
				break;
			case WtPageMeshingRuntimePhase::Ready:
				++snapshot.ready_records;
				break;
		}
		for (const Dependency &dependency : record.dependencies) {
			if (dependency.page) {
				++snapshot.pinned_pages;
			} else {
				++snapshot.unresolved_dependencies;
			}
			if (dependency.request_pending) {
				++snapshot.pending_dependency_requests;
			}
		}
	}
	return snapshot;
}

} // namespace world_transvoxel
