#pragma once

#include <cstdint>
#include <filesystem>

namespace world_transvoxel {

constexpr std::size_t kWtProductionSnapshotPageCapacity = 4096;
constexpr std::size_t kWtProductionSnapshotSourceByteCapacity =
	256U * 1024U * 1024U;

class WtAsyncStorageService;
class WtEditJournal;

enum class WtWorldSnapshotStoreStatus : std::uint8_t {
	Ok,
	InvalidInput,
	CapacityExceeded,
	OutputExists,
	ManifestFailure,
	PageFailure,
	JournalNotEmpty,
	JournalEmpty,
	CompactionFailure,
	IoFailure,
	PublishFailure,
};

struct WtWorldSnapshotStoreResult {
	std::filesystem::path manifest_path;
	std::uint64_t source_revision = 0;
	std::uint64_t world_revision = 0;
	std::size_t page_count = 0;
};

WtWorldSnapshotStoreStatus wt_write_migrated_world_snapshot(
	WtAsyncStorageService &storage,
	const WtEditJournal &journal,
	const std::filesystem::path &output_directory,
	WtWorldSnapshotStoreResult &output
);

WtWorldSnapshotStoreStatus wt_write_compacted_world_snapshot(
	WtAsyncStorageService &storage,
	const WtEditJournal &journal,
	const std::filesystem::path &output_directory,
	std::uint64_t new_source_revision,
	WtWorldSnapshotStoreResult &output
);

const char *wt_world_snapshot_store_status_message(
	WtWorldSnapshotStoreStatus status
) noexcept;

} // namespace world_transvoxel
