#include "services/wt_read_only_world_runtime.h"

#include "storage/wt_async_storage_service.h"
#include "storage/wt_edit_journal_store.h"

#include <limits>
#include <algorithm>
#include <utility>

namespace world_transvoxel {

bool WtReadOnlyWorldRuntime::enqueue_world_operation(
	WorldOperation &operation
) {
	std::lock_guard<std::mutex> lock(input_mutex_);
	if (world_operations_.size() >= world_operation_capacity_) return false;
	if (operation.kind != WorldOperationKind::Edit) {
		if (next_request_id_ ==
			static_cast<std::uint64_t>(
				std::numeric_limits<std::int64_t>::max()
			)) {
			return false;
		}
		operation.request_id = ++next_request_id_;
	}
	if (operation.kind == WorldOperationKind::Edit) {
		const auto insertion = std::find_if(
			world_operations_.begin(),
			world_operations_.end(),
			[](const WorldOperation &queued) {
				return queued.kind != WorldOperationKind::Edit;
			}
		);
		world_operations_.insert(insertion, std::move(operation));
		notify_work();
		return true;
	}
	world_operations_.push_back(std::move(operation));
	notify_work();
	return true;
}

bool WtReadOnlyWorldRuntime::has_pending_edit_operation() {
	std::lock_guard<std::mutex> lock(input_mutex_);
	return std::any_of(
		world_operations_.begin(),
		world_operations_.end(),
		[](const WorldOperation &operation) {
			return operation.kind == WorldOperationKind::Edit;
		}
	);
}

WtReadOnlyRuntimeStatus
WtReadOnlyWorldRuntime::request_authoritative_sample(
	const WtGridPoint &point,
	std::uint8_t lod,
	std::uint64_t &request_id
) {
	request_id = 0;
	if (!valid_ || edit_journal_store_ == nullptr ||
		lod > kWtMaximumLod) {
		return WtReadOnlyRuntimeStatus::InvalidQuery;
	}
	WorldOperation operation;
	operation.kind = WorldOperationKind::AuthoritativeSample;
	operation.point = point;
	operation.lod = lod;
	if (!enqueue_world_operation(operation)) {
		return WtReadOnlyRuntimeStatus::OperationQueueFull;
	}
	request_id = operation.request_id;
	return WtReadOnlyRuntimeStatus::Ok;
}

WtReadOnlyRuntimeStatus
WtReadOnlyWorldRuntime::request_authoritative_samples(
	const std::vector<WtGridPoint> &points,
	std::uint8_t lod,
	std::uint64_t &request_id
) {
	request_id = 0;
	if (!valid_ || edit_journal_store_ == nullptr ||
		lod > kWtMaximumLod || points.empty() ||
		points.size() > kWtMaximumAuthoritativeSampleBatchPoints) {
		return WtReadOnlyRuntimeStatus::InvalidQuery;
	}
	WorldOperation operation;
	operation.kind = WorldOperationKind::AuthoritativeSampleBatch;
	operation.points = points;
	operation.lod = lod;
	if (!enqueue_world_operation(operation)) {
		return WtReadOnlyRuntimeStatus::OperationQueueFull;
	}
	request_id = operation.request_id;
	return WtReadOnlyRuntimeStatus::Ok;
}

WtReadOnlyRuntimeStatus WtReadOnlyWorldRuntime::request_world_snapshot(
	const std::filesystem::path &output_directory,
	std::uint64_t new_source_revision,
	bool compact,
	std::uint64_t &request_id
) {
	request_id = 0;
	if (!valid_ || edit_journal_store_ == nullptr ||
		output_directory.empty() ||
		(compact && new_source_revision <= storage_.source_revision()) ||
		(!compact && new_source_revision != 0)) {
		return WtReadOnlyRuntimeStatus::InvalidSnapshot;
	}
	WorldOperation operation;
	operation.kind = compact ?
		WorldOperationKind::CompactSnapshot :
		WorldOperationKind::MigrateSnapshot;
	operation.output_directory = output_directory;
	operation.new_source_revision = new_source_revision;
	if (!enqueue_world_operation(operation)) {
		return WtReadOnlyRuntimeStatus::OperationQueueFull;
	}
	request_id = operation.request_id;
	return WtReadOnlyRuntimeStatus::Ok;
}

bool WtReadOnlyWorldRuntime::process_world_operation_event() {
	WorldOperation operation;
	{
		std::lock_guard<std::mutex> lock(input_mutex_);
		if (world_operations_.empty()) return false;
		operation = std::move(world_operations_.front());
		world_operations_.erase(world_operations_.begin());
	}
	switch (operation.kind) {
		case WorldOperationKind::Edit:
			return process_edit_operation(operation.transaction);
		case WorldOperationKind::AuthoritativeSample:
			return process_sample_query_operation(operation);
		case WorldOperationKind::AuthoritativeSampleBatch:
			return process_sample_batch_query_operation(operation);
		case WorldOperationKind::CompactSnapshot:
		case WorldOperationKind::MigrateSnapshot:
			return process_snapshot_operation(operation);
	}
	return true;
}

bool WtReadOnlyWorldRuntime::process_sample_query_operation(
	const WorldOperation &operation
) {
	WtAuthoritativeSample sample;
	const WtAuthoritativeSampleQueryStatus status =
		wt_query_authoritative_sample(
			operation.point,
			operation.lod,
			storage_,
			edit_journal_store_->journal(),
			initial_world_revision_,
			sample
		);
	WtReadOnlyPublication publication;
	publication.kind = status == WtAuthoritativeSampleQueryStatus::Ok ?
		WtReadOnlyPublicationKind::AuthoritativeSampleReady :
		WtReadOnlyPublicationKind::AuthoritativeSampleRejected;
	publication.request_id = operation.request_id;
	publication.sample_status = status;
	publication.authoritative_sample = sample;
	publication.world_revision = world_revision_.load();
	if (!push_publication(std::move(publication)) &&
		!stop_requested_.load()) {
		set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
	}
	{
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		if (status == WtAuthoritativeSampleQueryStatus::Ok) {
			++metrics_.sample_queries;
		} else {
			++metrics_.sample_query_rejections;
		}
	}
	return true;
}

bool WtReadOnlyWorldRuntime::process_sample_batch_query_operation(
	const WorldOperation &operation
) {
	std::vector<WtAuthoritativeSample> samples;
	const WtAuthoritativeSampleQueryStatus status =
		wt_query_authoritative_samples(
			operation.points,
			operation.lod,
			storage_,
			edit_journal_store_->journal(),
			initial_world_revision_,
			samples
		);
	WtReadOnlyPublication publication;
	publication.kind = status == WtAuthoritativeSampleQueryStatus::Ok ?
		WtReadOnlyPublicationKind::AuthoritativeSampleBatchReady :
		WtReadOnlyPublicationKind::AuthoritativeSampleBatchRejected;
	publication.request_id = operation.request_id;
	publication.sample_status = status;
	publication.authoritative_samples = std::move(samples);
	publication.world_revision = world_revision_.load();
	if (!push_publication(std::move(publication)) &&
		!stop_requested_.load()) {
		set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
	}
	{
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		if (status == WtAuthoritativeSampleQueryStatus::Ok) {
			metrics_.sample_queries += operation.points.size();
		} else {
			metrics_.sample_query_rejections +=
				operation.points.empty() ? 1U : operation.points.size();
		}
	}
	return true;
}

bool WtReadOnlyWorldRuntime::process_snapshot_operation(
	const WorldOperation &operation
) {
	WtWorldSnapshotStoreResult result;
	const bool compact =
		operation.kind == WorldOperationKind::CompactSnapshot;
	const WtWorldSnapshotStoreStatus status = compact ?
		wt_write_compacted_world_snapshot(
			storage_,
			edit_journal_store_->journal(),
			operation.output_directory,
			operation.new_source_revision,
			result
		) :
		wt_write_migrated_world_snapshot(
			storage_,
			edit_journal_store_->journal(),
			operation.output_directory,
			result
		);
	WtReadOnlyPublication publication;
	publication.kind = status == WtWorldSnapshotStoreStatus::Ok ?
		WtReadOnlyPublicationKind::WorldSnapshotReady :
		WtReadOnlyPublicationKind::WorldSnapshotRejected;
	publication.request_id = operation.request_id;
	publication.snapshot_status = status;
	publication.snapshot_manifest_path = std::move(result.manifest_path);
	publication.snapshot_source_revision = result.source_revision;
	publication.world_revision = result.world_revision;
	publication.snapshot_page_count = result.page_count;
	if (!push_publication(std::move(publication)) &&
		!stop_requested_.load()) {
		set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
	}
	{
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		if (status == WtWorldSnapshotStoreStatus::Ok) {
			++metrics_.world_snapshots;
		} else {
			++metrics_.world_snapshot_rejections;
		}
	}
	return true;
}

} // namespace world_transvoxel
