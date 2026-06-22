#include "services/wt_authoritative_sample_query.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_chunk_page.h"
#include "storage/wt_edit_journal_store.h"
#include "storage/wt_hash256.h"
#include "storage/wt_world_snapshot_store.h"
#include "wt_production_world_fixture.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace wt = world_transvoxel;
namespace wtt = world_transvoxel::testing;

namespace {

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

wt::WtId128 id(std::uint8_t seed) {
	wt::WtId128 output{};
	for (std::size_t index = 0; index < output.size(); ++index) {
		output[index] = static_cast<std::uint8_t>(seed + index);
	}
	return output;
}

wt::WtEditTransaction edit() {
	wt::WtEditTransaction transaction;
	transaction.source_revision = 7001;
	transaction.transaction_id = id(1);
	transaction.base_revision = 12;
	transaction.committed_revision = 13;
	transaction.author_id = 41;
	wt::WtEditCommand command;
	command.command_id = id(32);
	command.sequence = 0;
	command.world_revision = 13;
	command.operation = wt::WtEditOperation::SetDensity;
	command.shape = wt::WtEditShape::AxisAlignedBox;
	command.density_value = 10.0F;
	command.box = {
		-2 * wt::kWtEditCoordinateScale,
		-2 * wt::kWtEditCoordinateScale,
		-2 * wt::kWtEditCoordinateScale,
		18 * wt::kWtEditCoordinateScale,
		18 * wt::kWtEditCoordinateScale,
		18 * wt::kWtEditCoordinateScale,
	};
	check(wt::wt_edit_box_bounds(command.box, command.bounds),
		"snapshot query edit bounds failed");
	transaction.commands.push_back(command);
	return transaction;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path &path) {
	std::ifstream input(path, std::ios::binary);
	return {
		std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>(),
	};
}

bool write_file(
	const std::filesystem::path &path,
	const std::vector<std::uint8_t> &bytes
) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write(
		reinterpret_cast<const char *>(bytes.data()),
		static_cast<std::streamsize>(bytes.size())
	);
	return output.good();
}

std::filesystem::path write_conflicting_world(
	const std::filesystem::path &root,
	const std::filesystem::path &source_world
) {
	const std::vector<std::uint8_t> world_bytes = read_file(source_world);
	wt::WtWorldManifestView view;
	check(wt::wt_open_world_manifest(
		{ world_bytes.data(), world_bytes.size() },
		view
	) == wt::WtWorldManifestStatus::Ok,
		"conflicting query manifest open failed");
	wt::WtWorldManifest manifest;
	manifest.source_revision = view.source_revision;
	manifest.world_revision = view.world_revision;
	manifest.configuration_hash = view.configuration_hash;
	manifest.dependencies = view.dependencies;
	manifest.pages = view.pages;
	const wt::WtChunkKey key { -1, 0, 0, 0 };
	auto entry = std::find_if(
		manifest.pages.begin(),
		manifest.pages.end(),
		[&](const wt::WtWorldPageIndexEntry &candidate) {
			return candidate.key == key;
		}
	);
	check(entry != manifest.pages.end(),
		"conflicting query source page missing");
	if (entry == manifest.pages.end()) return {};
	const std::vector<std::uint8_t> page_bytes = read_file(
		wt::wt_page_object_path(root, entry->content_hash)
	);
	wt::WtChunkPageView page_view;
	wt::WtChunkPage page;
	check(wt::wt_open_chunk_page(
		{ page_bytes.data(), page_bytes.size() },
		page_view
	) == wt::WtChunkPageStatus::Ok &&
		wt::wt_decode_chunk_page(page_view, page) ==
			wt::WtChunkPageStatus::Ok,
		"conflicting query source page decode failed");
	const std::size_t sample_index = (9U * 19U + 9U) * 19U + 17U;
	page.samples[sample_index].density += 1.0F;
	std::vector<std::uint8_t> conflicting_page;
	check(wt::wt_write_chunk_page(page, conflicting_page) ==
		wt::WtChunkPageStatus::Ok,
		"conflicting query page write failed");
	entry->byte_size = conflicting_page.size();
	entry->content_hash = wt::wt_sha256(
		conflicting_page.data(), conflicting_page.size()
	);
	check(write_file(
		wt::wt_page_object_path(root, entry->content_hash),
		conflicting_page
	), "conflicting query page object write failed");
	std::vector<std::uint8_t> conflicting_world;
	check(wt::wt_write_world_manifest(manifest, conflicting_world) ==
		wt::WtWorldManifestStatus::Ok,
		"conflicting query manifest write failed");
	const std::filesystem::path path = root / "conflicting.wtworld";
	check(write_file(path, conflicting_world),
		"conflicting query manifest file write failed");
	return path;
}

void append_snapshot(
	const std::filesystem::path &root,
	std::vector<std::uint8_t> &evidence
) {
	const std::vector<std::uint8_t> world = read_file(root / "world.wtworld");
	evidence.insert(evidence.end(), world.begin(), world.end());
	wt::WtWorldManifestView view;
	check(wt::wt_open_world_manifest(
		{ world.data(), world.size() },
		view
	) == wt::WtWorldManifestStatus::Ok,
		"snapshot evidence manifest failed");
	for (const wt::WtWorldPageIndexEntry &entry : view.pages) {
		const std::vector<std::uint8_t> page = read_file(
			wt::wt_page_object_path(root, entry.content_hash)
		);
		evidence.insert(evidence.end(), page.begin(), page.end());
	}
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

class FixtureRoot {
public:
	FixtureRoot() {
		path = std::filesystem::temp_directory_path() /
			("wt_production_snapshot_query_" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()
			));
	}
	~FixtureRoot() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
	std::filesystem::path path;
};

} // namespace

int main() {
	FixtureRoot fixture;
	const std::filesystem::path source_root = fixture.path / "source";
	const std::filesystem::path compacted_root = fixture.path / "compacted";
	const std::filesystem::path migrated_root = fixture.path / "migrated";
	std::filesystem::create_directories(fixture.path);
	std::filesystem::path world_path;
	check(wtt::wt_write_production_streaming_fixture(
		source_root, 7001, 12, world_path
	), "snapshot query fixture write failed");

	wt::WtAsyncStorageService storage({ 16, 16, wt::kWtMaximumContainerSize });
	check(storage.open(world_path, source_root) ==
		wt::WtAsyncStorageStatus::Ok,
		"snapshot query storage open failed");
	wt::WtEditJournalStore journal;
	check(journal.open(source_root / "world.wtedit", 7001, 12) ==
		wt::WtEditJournalStoreStatus::Ok,
		"snapshot query journal open failed");

	wt::WtAuthoritativeSample sample;
	check(wt::wt_query_authoritative_sample(
		{ 8, 8, 8 }, 0, storage, journal.journal(), 12, sample
	) == wt::WtAuthoritativeSampleQueryStatus::Ok &&
		sample.sample.density == -0.25F &&
		sample.sample.material == 7 &&
		sample.source_revision == 7001 &&
		sample.world_revision == 12,
		"initial authoritative sample mismatch");
	check(wt::wt_query_authoritative_sample(
		{ 0, 8, 8 }, 0, storage, journal.journal(), 12, sample
	) == wt::WtAuthoritativeSampleQueryStatus::Ok &&
		sample.agreeing_page_count == 2,
		"overlapping authoritative pages did not agree");
	check(wt::wt_query_authoritative_sample(
		{ 1, 8, 8 }, 1, storage, journal.journal(), 12, sample
	) == wt::WtAuthoritativeSampleQueryStatus::InvalidPoint,
		"misaligned authoritative sample was accepted");
	check(wt::wt_query_authoritative_sample(
		{ 4096, 8, 8 }, 0, storage, journal.journal(), 12, sample
	) == wt::WtAuthoritativeSampleQueryStatus::NotFound,
		"out-of-world authoritative sample was found");

	check(journal.append(edit()) == wt::WtEditJournalStoreStatus::Ok,
		"snapshot query durable edit failed");
	check(wt::wt_query_authoritative_sample(
		{ 8, 8, 8 }, 0, storage, journal.journal(), 12, sample
	) == wt::WtAuthoritativeSampleQueryStatus::Ok &&
		sample.sample.density == 10.0F &&
		sample.world_revision == 13,
		"edited authoritative sample mismatch");

	wt::WtWorldSnapshotStoreResult result;
	check(wt::wt_write_migrated_world_snapshot(
		storage, journal.journal(), migrated_root, result
	) == wt::WtWorldSnapshotStoreStatus::JournalNotEmpty,
		"migration accepted a nonempty journal");
	check(wt::wt_write_compacted_world_snapshot(
		storage, journal.journal(), compacted_root, 7002, result
	) == wt::WtWorldSnapshotStoreStatus::Ok &&
		result.source_revision == 7002 &&
		result.world_revision == 13 &&
		result.page_count == 4,
		"compacted snapshot publication failed");
	check(wt::wt_write_compacted_world_snapshot(
		storage, journal.journal(), compacted_root, 7003, result
	) == wt::WtWorldSnapshotStoreStatus::OutputExists,
		"snapshot overwrote an existing output directory");
	storage.close();
	journal.close();

	const std::filesystem::path conflicting_world =
		write_conflicting_world(source_root, world_path);
	wt::WtAsyncStorageService conflicting_storage({
		16, 16, wt::kWtMaximumContainerSize
	});
	check(conflicting_storage.open(conflicting_world, source_root) ==
		wt::WtAsyncStorageStatus::Ok,
		"conflicting query storage open failed");
	wt::WtEditJournal conflicting_journal(1, 1, 4096);
	conflicting_journal.reset(7001, 12);
	check(wt::wt_query_authoritative_sample(
		{ 0, 8, 8 },
		0,
		conflicting_storage,
		conflicting_journal,
		12,
		sample
	) == wt::WtAuthoritativeSampleQueryStatus::ConflictingSamples,
		"conflicting overlapping authoritative pages were accepted");
	conflicting_storage.close();

	const std::filesystem::path capacity_root = fixture.path / "capacity";
	std::filesystem::create_directories(capacity_root);
	const std::vector<std::uint8_t> source_world_bytes = read_file(world_path);
	wt::WtWorldManifestView source_world_view;
	check(wt::wt_open_world_manifest(
		{ source_world_bytes.data(), source_world_bytes.size() },
		source_world_view
	) == wt::WtWorldManifestStatus::Ok,
		"snapshot capacity source manifest open failed");
	wt::WtWorldManifest capacity_manifest;
	capacity_manifest.source_revision = source_world_view.source_revision;
	capacity_manifest.world_revision = source_world_view.world_revision;
	capacity_manifest.configuration_hash =
		source_world_view.configuration_hash;
	capacity_manifest.dependencies = source_world_view.dependencies;
	capacity_manifest.pages = source_world_view.pages;
	for (std::int32_t x = 3;
		capacity_manifest.pages.size() <=
			wt::kWtProductionSnapshotPageCapacity;
		++x) {
		wt::WtWorldPageIndexEntry entry;
		entry.key.x = x;
		entry.byte_size = source_world_view.pages.front().byte_size;
		entry.content_hash = source_world_view.pages.front().content_hash;
		capacity_manifest.pages.push_back(entry);
	}
	std::vector<std::uint8_t> capacity_world;
	check(wt::wt_write_world_manifest(capacity_manifest, capacity_world) ==
		wt::WtWorldManifestStatus::Ok &&
		write_file(capacity_root / "world.wtworld", capacity_world),
		"snapshot capacity fixture write failed");
	wt::WtAsyncStorageService capacity_storage({
		16, 16, wt::kWtMaximumContainerSize
	});
	check(capacity_storage.open(
		capacity_root / "world.wtworld", capacity_root
	) == wt::WtAsyncStorageStatus::Ok,
		"snapshot capacity fixture open failed");
	wt::WtEditJournal capacity_journal(1, 1, 4096);
	capacity_journal.reset(7001, 12);
	check(wt::wt_write_migrated_world_snapshot(
		capacity_storage,
		capacity_journal,
		fixture.path / "capacity-output",
		result
	) == wt::WtWorldSnapshotStoreStatus::CapacityExceeded,
		"snapshot page capacity overflow was accepted");
	capacity_storage.close();

	wt::WtAsyncStorageService compacted_storage({
		16, 16, wt::kWtMaximumContainerSize
	});
	check(compacted_storage.open(
		compacted_root / "world.wtworld",
		compacted_root
	) == wt::WtAsyncStorageStatus::Ok,
		"compacted snapshot storage open failed");
	wt::WtEditJournalStore compacted_journal;
	check(compacted_journal.open(
		compacted_root / "world.wtedit", 7002, 13
	) == wt::WtEditJournalStoreStatus::Ok,
		"compacted snapshot journal initialization failed");
	check(wt::wt_query_authoritative_sample(
		{ 8, 8, 8 },
		0,
		compacted_storage,
		compacted_journal.journal(),
		13,
		sample
	) == wt::WtAuthoritativeSampleQueryStatus::Ok &&
		sample.sample.density == 10.0F &&
		sample.source_revision == 7002 &&
		sample.world_revision == 13,
		"compacted authoritative sample mismatch");
	check(wt::wt_write_migrated_world_snapshot(
		compacted_storage,
		compacted_journal.journal(),
		migrated_root,
		result
	) == wt::WtWorldSnapshotStoreStatus::Ok &&
		result.source_revision == 7002 &&
		result.world_revision == 13,
		"current-schema migration snapshot failed");
	compacted_storage.close();
	compacted_journal.close();

	std::vector<std::uint8_t> evidence;
	append_snapshot(compacted_root, evidence);
	append_snapshot(migrated_root, evidence);
	if (failure_count != 0) {
		std::fprintf(stderr,
			"PRODUCTION_SNAPSHOT_QUERY_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("PRODUCTION_SNAPSHOT_QUERY_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"PRODUCTION_SNAPSHOT_QUERY_PASS queries=7 compactions=1 "
		"migrations=1 rejection_cases=6\n"
	);
	return 0;
}
