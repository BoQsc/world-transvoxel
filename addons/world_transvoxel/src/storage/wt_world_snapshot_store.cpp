#include "storage/wt_world_snapshot_store.h"

#include "bake/wt_snapshot_compactor.h"
#include "editing/wt_edit_journal.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_hash256.h"
#include "storage/wt_world_manifest.h"

#include <cstdio>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace world_transvoxel {
namespace {

bool flush_durable(FILE *file) noexcept {
	if (file == nullptr || std::fflush(file) != 0) return false;
#if defined(_WIN32)
	return _commit(_fileno(file)) == 0;
#else
	return fsync(fileno(file)) == 0;
#endif
}

FILE *open_write(const std::filesystem::path &path) {
#if defined(_WIN32)
	return _wfopen(path.c_str(), L"wb");
#else
	return std::fopen(path.c_str(), "wb");
#endif
}

bool write_file(
	const std::filesystem::path &path,
	const std::vector<std::uint8_t> &bytes
) {
	FILE *file = open_write(path);
	if (file == nullptr) return false;
	const bool written = bytes.empty() ||
		std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
	const bool durable = written && flush_durable(file);
	const bool closed = std::fclose(file) == 0;
	return durable && closed;
}

bool sync_directory(const std::filesystem::path &path) noexcept {
#if defined(_WIN32)
	(void)path;
	return true;
#else
	const int descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY);
	if (descriptor < 0) return false;
	const bool ok = fsync(descriptor) == 0;
	close(descriptor);
	return ok;
#endif
}

std::string hash_hex(const WtHash256 &hash) {
	constexpr char digits[] = "0123456789abcdef";
	std::string output(hash.size() * 2, '0');
	for (std::size_t index = 0; index < hash.size(); ++index) {
		output[index * 2] = digits[hash[index] >> 4];
		output[index * 2 + 1] = digits[hash[index] & 0x0fU];
	}
	return output;
}

WtWorldSnapshotStoreStatus read_source(
	WtAsyncStorageService &storage,
	std::vector<std::uint8_t> &world_bytes,
	WtWorldManifestView &world,
	std::vector<WtBakedChunkPage> &pages
) {
	if (!storage.snapshot_manifest(world_bytes) ||
		wt_open_world_manifest(
			{ world_bytes.data(), world_bytes.size() },
			world
		) != WtWorldManifestStatus::Ok) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	if (world.pages.size() > kWtProductionSnapshotPageCapacity) {
		return WtWorldSnapshotStoreStatus::CapacityExceeded;
	}
	std::size_t source_bytes = 0;
	for (const WtWorldPageIndexEntry &entry : world.pages) {
		if (entry.byte_size >
			kWtProductionSnapshotSourceByteCapacity - source_bytes) {
			return WtWorldSnapshotStoreStatus::CapacityExceeded;
		}
		source_bytes += static_cast<std::size_t>(entry.byte_size);
	}
	pages.clear();
	pages.reserve(world.pages.size());
	for (const WtWorldPageIndexEntry &entry : world.pages) {
		std::shared_ptr<const std::vector<std::uint8_t>> bytes;
		if (storage.load_page_now(entry.key, bytes) != WtPageLoadStatus::Ok ||
			!bytes) {
			return WtWorldSnapshotStoreStatus::PageFailure;
		}
		WtBakedChunkPage page;
		page.key = entry.key;
		page.content_hash = entry.content_hash;
		page.bytes = *bytes;
		pages.push_back(std::move(page));
	}
	return WtWorldSnapshotStoreStatus::Ok;
}

WtWorldSnapshotStoreStatus publish(
	const std::filesystem::path &output_directory,
	const std::vector<std::uint8_t> &world_bytes,
	const std::vector<WtBakedChunkPage> &pages,
	WtWorldSnapshotStoreResult &output
) {
	output = {};
	if (output_directory.empty() || output_directory.filename().empty()) {
		return WtWorldSnapshotStoreStatus::InvalidInput;
	}
	std::error_code error;
	const std::filesystem::path parent = output_directory.parent_path().empty() ?
		std::filesystem::current_path(error) :
		output_directory.parent_path();
	if (error || !std::filesystem::is_directory(parent, error) || error) {
		return WtWorldSnapshotStoreStatus::InvalidInput;
	}
	if (std::filesystem::exists(output_directory, error) || error) {
		return error ? WtWorldSnapshotStoreStatus::IoFailure :
			WtWorldSnapshotStoreStatus::OutputExists;
	}
	WtWorldManifestView view;
	if (wt_open_world_manifest(
			{ world_bytes.data(), world_bytes.size() },
			view
		) != WtWorldManifestStatus::Ok ||
		view.pages.size() != pages.size()) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	const std::filesystem::path temporary =
		parent / (output_directory.filename().string() + ".tmp");
	if (std::filesystem::exists(temporary, error) || error ||
		!std::filesystem::create_directory(temporary, error) || error) {
		return WtWorldSnapshotStoreStatus::IoFailure;
	}
	bool wrote_all = write_file(temporary / "world.wtworld", world_bytes);
	for (const WtBakedChunkPage &page : pages) {
		wrote_all = wrote_all && write_file(
			temporary / (hash_hex(page.content_hash) + ".wtchunk"),
			page.bytes
		);
	}
	if (wrote_all) wrote_all = sync_directory(temporary);
	if (!wrote_all) {
		std::filesystem::remove_all(temporary, error);
		return WtWorldSnapshotStoreStatus::IoFailure;
	}
	std::filesystem::rename(temporary, output_directory, error);
	if (error) {
		std::filesystem::remove_all(temporary, error);
		return WtWorldSnapshotStoreStatus::PublishFailure;
	}
	if (!sync_directory(parent)) {
		std::filesystem::remove_all(output_directory, error);
		return WtWorldSnapshotStoreStatus::PublishFailure;
	}
	output.manifest_path = output_directory / "world.wtworld";
	output.source_revision = view.source_revision;
	output.world_revision = view.world_revision;
	output.page_count = view.pages.size();
	return WtWorldSnapshotStoreStatus::Ok;
}

} // namespace

WtWorldSnapshotStoreStatus wt_write_migrated_world_snapshot(
	WtAsyncStorageService &storage,
	const WtEditJournal &journal,
	const std::filesystem::path &output_directory,
	WtWorldSnapshotStoreResult &output
) {
	output = {};
	if (!journal.initialized() ||
		journal.source_revision() != storage.source_revision() ||
		journal.initial_world_revision() != storage.world_revision()) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	if (journal.transaction_count() != 0) {
		return WtWorldSnapshotStoreStatus::JournalNotEmpty;
	}
	std::vector<std::uint8_t> previous_world;
	WtWorldManifestView view;
	std::vector<WtBakedChunkPage> pages;
	const WtWorldSnapshotStoreStatus read = read_source(
		storage, previous_world, view, pages
	);
	if (read != WtWorldSnapshotStoreStatus::Ok) return read;
	WtWorldManifest manifest;
	manifest.source_revision = view.source_revision;
	manifest.world_revision = view.world_revision;
	manifest.configuration_hash = view.configuration_hash;
	manifest.dependencies = view.dependencies;
	manifest.pages = view.pages;
	std::vector<std::uint8_t> migrated_world;
	if (wt_write_world_manifest(manifest, migrated_world) !=
		WtWorldManifestStatus::Ok) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	return publish(output_directory, migrated_world, pages, output);
}

WtWorldSnapshotStoreStatus wt_write_compacted_world_snapshot(
	WtAsyncStorageService &storage,
	const WtEditJournal &journal,
	const std::filesystem::path &output_directory,
	std::uint64_t new_source_revision,
	WtWorldSnapshotStoreResult &output
) {
	output = {};
	if (!journal.initialized() ||
		journal.source_revision() != storage.source_revision() ||
		journal.initial_world_revision() != storage.world_revision()) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	if (journal.transaction_count() == 0) {
		return WtWorldSnapshotStoreStatus::JournalEmpty;
	}
	std::vector<std::uint8_t> previous_world;
	WtWorldManifestView view;
	std::vector<WtBakedChunkPage> pages;
	const WtWorldSnapshotStoreStatus read = read_source(
		storage, previous_world, view, pages
	);
	if (read != WtWorldSnapshotStoreStatus::Ok) return read;
	WtCompactedSnapshot compacted;
	if (wt_compact_snapshot(
			{ previous_world.data(), previous_world.size() },
			pages,
			journal,
			new_source_revision,
			view.pages.size(),
			compacted
		) != WtSnapshotCompactionStatus::Ok) {
		return WtWorldSnapshotStoreStatus::CompactionFailure;
	}
	return publish(
		output_directory,
		compacted.world_bytes,
		compacted.pages,
		output
	);
}

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
	}
	return "unknown world snapshot status";
}

} // namespace world_transvoxel
