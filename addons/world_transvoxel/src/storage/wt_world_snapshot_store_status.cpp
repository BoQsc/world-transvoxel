#include "storage/wt_world_snapshot_store.h"

namespace world_transvoxel {

const char *wt_world_snapshot_store_status_message(
	WtWorldSnapshotStoreStatus status
) noexcept {
	switch (status) {
		case WtWorldSnapshotStoreStatus::Ok: return "ok";
		case WtWorldSnapshotStoreStatus::InvalidInput:
			return "world snapshot output path or revision is invalid";
		case WtWorldSnapshotStoreStatus::CapacityExceeded:
			return "world snapshot page or byte capacity is exceeded";
		case WtWorldSnapshotStoreStatus::OutputExists:
			return "world snapshot output directory already exists";
		case WtWorldSnapshotStoreStatus::ManifestFailure:
			return "world snapshot manifest validation failed";
		case WtWorldSnapshotStoreStatus::PageFailure:
			return "world snapshot page loading or validation failed";
		case WtWorldSnapshotStoreStatus::JournalNotEmpty:
			return "world migration requires an empty edit journal";
		case WtWorldSnapshotStoreStatus::JournalEmpty:
			return "world compaction requires committed edits";
		case WtWorldSnapshotStoreStatus::CompactionFailure:
			return "world snapshot compaction failed";
		case WtWorldSnapshotStoreStatus::IoFailure:
			return "world snapshot file writing failed";
		case WtWorldSnapshotStoreStatus::PublishFailure:
			return "world snapshot directory publication failed";
		case WtWorldSnapshotStoreStatus::AllocationFailure:
			return "world snapshot workspace allocation failed";
	}
	return "unknown world snapshot status";
}

} // namespace world_transvoxel
