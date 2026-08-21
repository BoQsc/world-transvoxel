#include "services/wt_authoritative_sample_query.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_chunk_page.h"
#include "storage/wt_edit_journal_store.h"
#include "storage/wt_hash256.h"
#include "storage/wt_procedural_snapshot_descriptor.h"
#include "storage/wt_procedural_world_source.h"
#include "storage/wt_world_snapshot_store.h"
#include "wt_production_world_fixture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
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

wt::WtEditTransaction sparse_edit(
	std::uint64_t source_revision,
	std::uint64_t base_revision,
	std::uint64_t committed_revision,
	std::uint8_t identity_seed,
	const wt::WtGridPoint &center,
	float density
) {
	wt::WtEditTransaction transaction;
	transaction.source_revision = source_revision;
	transaction.transaction_id = id(identity_seed);
	transaction.base_revision = base_revision;
	transaction.committed_revision = committed_revision;
	transaction.author_id = 42;
	wt::WtEditCommand command;
	command.command_id = id(static_cast<std::uint8_t>(identity_seed + 32));
	command.sequence = 0;
	command.world_revision = committed_revision;
	command.operation = wt::WtEditOperation::SetDensity;
	command.shape = wt::WtEditShape::AxisAlignedBox;
	command.density_value = density;
	const std::int64_t scale = wt::kWtEditCoordinateScale;
	command.box = {
		(center.x - 2) * scale,
		(center.y - 2) * scale,
		(center.z - 2) * scale,
		(center.x + 2) * scale,
		(center.y + 2) * scale,
		(center.z + 2) * scale,
	};
	check(wt::wt_edit_box_bounds(command.box, command.bounds),
		"sparse procedural edit bounds failed");
	transaction.commands.push_back(command);
	return transaction;
}

std::uint64_t percentile(
	std::vector<std::uint64_t> samples,
	double fraction
) {
	if (samples.empty()) return 0;
	std::sort(samples.begin(), samples.end());
	const std::size_t index = static_cast<std::size_t>(
		fraction * static_cast<double>(samples.size() - 1)
	);
	return samples[index];
}

void run_sparse_hierarchy_lookup_contract(
	const wt::WtProceduralWorldDescriptor &descriptor
) {
	const auto implicit_started = std::chrono::steady_clock::now();
	wt::WtPageHierarchy implicit =
		wt::WtPageHierarchy::implicit_procedural(descriptor);
	const auto implicit_finished = std::chrono::steady_clock::now();
	const auto flat_started = std::chrono::steady_clock::now();
	std::vector<wt::WtChunkKey> flat_keys = wt::wt_procedural_keys(descriptor);
	wt::WtPageHierarchy flat =
		wt::WtPageHierarchy::explicit_catalog(std::move(flat_keys));
	const auto flat_finished = std::chrono::steady_clock::now();
	check(implicit.valid() && flat.valid() &&
		implicit.page_count() == 299520 &&
		flat.page_count() == implicit.page_count(),
		"sparse hierarchy declared page contract failed");
	check(implicit.metrics().explicit_index_entries == 0 &&
		implicit.metrics().estimated_index_bytes <= 64 &&
		flat.metrics().explicit_index_entries == 299520 &&
		flat.metrics().estimated_index_bytes >= 299520 * sizeof(wt::WtChunkKey),
		"sparse hierarchy memory accounting failed");
	check(implicit.contains({ 8, 0, 8, 0 }) &&
		!implicit.contains({ -1, 0, 8, 0 }),
		"sparse hierarchy presence boundary failed");
	wt::WtChunkKey ancestor;
	check(implicit.ancestor({ 8, 0, 8, 0 }, 3, ancestor) &&
		ancestor == wt::WtChunkKey { 1, 0, 1, 3 },
		"sparse hierarchy ancestor query failed");
	std::array<wt::WtChunkKey, 8> children{};
	check(implicit.complete_children(ancestor, children),
		"sparse hierarchy child query failed");
	std::vector<wt::WtChunkKey> query;
	check(implicit.append_face_neighbors({ 8, 0, 8, 0 }, query, 6) &&
		query.size() == 6,
		"sparse hierarchy neighbor query failed");
	check(implicit.query_range(
			{ 8, 0, 8, 0 }, { 9, 1, 9, 0 }, query, 8
		) && query.size() == 8,
		"sparse hierarchy range query failed");
	check(implicit.query_viewer_roots(
			{ 8, 0, 8, 3 }, 1, query, 27
		) && query.size() == 18,
		"sparse hierarchy viewer-root query failed");
	const wt::WtChunkKey incomplete_parent { 1, 1, 1, 1 };
	std::vector<wt::WtChunkKey> incomplete_catalog = {
		incomplete_parent,
		{ 2, 2, 2, 0 }, { 3, 2, 2, 0 }, { 2, 3, 2, 0 },
		{ 3, 3, 2, 0 }, { 2, 2, 3, 0 }, { 3, 2, 3, 0 },
		{ 2, 3, 3, 0 },
	};
	wt::WtPageHierarchy incomplete =
		wt::WtPageHierarchy::explicit_catalog(incomplete_catalog);
	check(incomplete.valid() &&
		!incomplete.complete_children(incomplete_parent, children),
		"missing hierarchy descendant did not fail closed");
	wt::WtPageHierarchy missing_ancestor =
		wt::WtPageHierarchy::explicit_catalog({ { 2, 2, 2, 0 } });
	check(missing_ancestor.valid() &&
		!missing_ancestor.ancestor({ 2, 2, 2, 0 }, 1, ancestor),
		"missing hierarchy ancestor did not fail closed");
	wt::WtPageHierarchy duplicate = wt::WtPageHierarchy::explicit_catalog({
		{ 2, 2, 2, 0 }, { 2, 2, 2, 0 },
	});
	check(!duplicate.valid(), "duplicate hierarchy keys were accepted");

	std::vector<std::uint64_t> implicit_samples;
	std::vector<std::uint64_t> flat_samples;
	std::vector<std::uint64_t> ancestor_samples;
	std::vector<std::uint64_t> child_samples;
	std::vector<std::uint64_t> neighbor_samples;
	std::vector<std::uint64_t> range_samples;
	std::vector<std::uint64_t> viewer_root_samples;
	implicit_samples.reserve(4096);
	flat_samples.reserve(4096);
	ancestor_samples.reserve(4096);
	child_samples.reserve(4096);
	neighbor_samples.reserve(4096);
	range_samples.reserve(4096);
	viewer_root_samples.reserve(4096);
	bool lookup_agreement = true;
	for (std::uint32_t index = 0; index < 4096; ++index) {
		const std::uint8_t lod = static_cast<std::uint8_t>(index % 4U);
		wt::WtChunkKey minimum;
		wt::WtChunkKey maximum;
		check(wt::wt_procedural_lod_key_bounds(
			descriptor, lod, minimum, maximum
		), "sparse hierarchy lookup bounds failed");
		const std::uint32_t width_x = static_cast<std::uint32_t>(
			maximum.x - minimum.x + 1
		);
		const std::uint32_t width_y = static_cast<std::uint32_t>(
			maximum.y - minimum.y + 1
		);
		const std::uint32_t width_z = static_cast<std::uint32_t>(
			maximum.z - minimum.z + 1
		);
		const wt::WtChunkKey key {
			minimum.x + static_cast<std::int32_t>((index * 37U) % width_x),
			minimum.y + static_cast<std::int32_t>((index * 17U) % width_y),
			minimum.z + static_cast<std::int32_t>((index * 53U) % width_z),
			lod,
		};
		const auto implicit_begin = std::chrono::steady_clock::now();
		const bool implicit_present = implicit.contains(key);
		const auto implicit_end = std::chrono::steady_clock::now();
		const auto flat_begin = std::chrono::steady_clock::now();
		const bool flat_present = flat.contains(key);
		const auto flat_end = std::chrono::steady_clock::now();
		lookup_agreement = lookup_agreement &&
			implicit_present == flat_present && implicit_present;
		implicit_samples.push_back(static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				implicit_end - implicit_begin
			).count()
		));
		flat_samples.push_back(static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				flat_end - flat_begin
			).count()
		));
		wt::WtChunkKey measured_ancestor;
		const auto ancestor_begin = std::chrono::steady_clock::now();
		const bool ancestor_present = implicit.ancestor(
			key, wt::kWtProceduralMaximumLod, measured_ancestor
		);
		const auto ancestor_end = std::chrono::steady_clock::now();
		lookup_agreement = lookup_agreement && ancestor_present;
		ancestor_samples.push_back(static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				ancestor_end - ancestor_begin
			).count()
		));
		std::array<wt::WtChunkKey, 8> measured_children{};
		const auto child_begin = std::chrono::steady_clock::now();
		const bool children_present = implicit.complete_children(
			measured_ancestor, measured_children
		);
		const auto child_end = std::chrono::steady_clock::now();
		lookup_agreement = lookup_agreement && children_present;
		child_samples.push_back(static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				child_end - child_begin
			).count()
		));
		const auto neighbor_begin = std::chrono::steady_clock::now();
		const bool neighbors_present = implicit.append_face_neighbors(
			key, query, 6
		);
		const auto neighbor_end = std::chrono::steady_clock::now();
		lookup_agreement = lookup_agreement && neighbors_present;
		neighbor_samples.push_back(static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				neighbor_end - neighbor_begin
			).count()
		));
		const auto range_begin = std::chrono::steady_clock::now();
		const bool range_present = implicit.query_range(key, key, query, 1);
		const auto range_end = std::chrono::steady_clock::now();
		lookup_agreement = lookup_agreement && range_present && query.size() == 1;
		range_samples.push_back(static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				range_end - range_begin
			).count()
		));
		const auto viewer_root_begin = std::chrono::steady_clock::now();
		const bool viewer_roots_present = implicit.query_viewer_roots(
			measured_ancestor, 1, query, 27
		);
		const auto viewer_root_end = std::chrono::steady_clock::now();
		lookup_agreement = lookup_agreement && viewer_roots_present &&
			!query.empty();
		viewer_root_samples.push_back(static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				viewer_root_end - viewer_root_begin
			).count()
		));
	}
	check(lookup_agreement, "sparse and flat hierarchy lookup diverged");
	const auto implicit_startup_ns =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			implicit_finished - implicit_started
		).count();
	const auto flat_startup_ns =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			flat_finished - flat_started
		).count();
	std::printf(
		"SPARSE_HIERARCHY_TIMINGS implicit_startup_ns=%lld "
		"flat_startup_ns=%lld implicit_p50=%llu implicit_p95=%llu "
		"implicit_p99=%llu implicit_worst=%llu flat_p50=%llu flat_p95=%llu "
		"flat_p99=%llu flat_worst=%llu\n",
		static_cast<long long>(implicit_startup_ns),
		static_cast<long long>(flat_startup_ns),
		static_cast<unsigned long long>(percentile(implicit_samples, 0.50)),
		static_cast<unsigned long long>(percentile(implicit_samples, 0.95)),
		static_cast<unsigned long long>(percentile(implicit_samples, 0.99)),
		static_cast<unsigned long long>(percentile(implicit_samples, 1.00)),
		static_cast<unsigned long long>(percentile(flat_samples, 0.50)),
		static_cast<unsigned long long>(percentile(flat_samples, 0.95)),
		static_cast<unsigned long long>(percentile(flat_samples, 0.99)),
		static_cast<unsigned long long>(percentile(flat_samples, 1.00))
	);
	const auto print_distribution = [](
		const char *name,
		const std::vector<std::uint64_t> &samples
	) {
		std::printf(
			"SPARSE_HIERARCHY_OPERATION operation=%s p50_ns=%llu "
			"p95_ns=%llu p99_ns=%llu worst_ns=%llu samples=%zu\n",
			name,
			static_cast<unsigned long long>(percentile(samples, 0.50)),
			static_cast<unsigned long long>(percentile(samples, 0.95)),
			static_cast<unsigned long long>(percentile(samples, 0.99)),
			static_cast<unsigned long long>(percentile(samples, 1.00)),
			samples.size()
		);
	};
	print_distribution("ancestor", ancestor_samples);
	print_distribution("children", child_samples);
	print_distribution("neighbors", neighbor_samples);
	print_distribution("range", range_samples);
	print_distribution("viewer_roots", viewer_root_samples);
}

void run_sparse_procedural_snapshot_contract(
	const std::filesystem::path &root
) {
	const std::filesystem::path first_journal_root = root / "journal-first";
	const std::filesystem::path first_snapshot = root / "snapshot-first";
	const std::filesystem::path second_journal_root = root / "journal-second";
	const std::filesystem::path second_snapshot = root / "snapshot-second";
	const std::filesystem::path final_journal_root = root / "journal-final";
	const std::filesystem::path migrated_snapshot = root / "snapshot-migrated";
	const std::filesystem::path migrated_journal_root = root / "journal-migrated";
	const std::filesystem::path capacity_journal_root = root / "journal-capacity";
	const std::filesystem::path capacity_snapshot = root / "snapshot-capacity";
	const std::filesystem::path interrupted_snapshot = root / "snapshot-interrupted";
	std::filesystem::create_directories(first_journal_root);
	std::filesystem::create_directories(second_journal_root);
	std::filesystem::create_directories(final_journal_root);
	std::filesystem::create_directories(migrated_journal_root);
	std::filesystem::create_directories(capacity_journal_root);
	wt::WtProceduralWorldDescriptor descriptor;
	descriptor.chunk_count_x = 128;
	descriptor.chunk_count_y = 16;
	descriptor.chunk_count_z = 128;
	descriptor.chunk_y = -8;
	descriptor.source_revision = 90001;
	descriptor.world_revision = 0;
	descriptor.seed = 19021;
	descriptor.mode = wt::WtProceduralWorldMode::RollingHillsCave;
	descriptor.bottom_boundary_policy =
		wt::WtProceduralBottomBoundaryPolicy::Bedrock;
	descriptor.bottom_boundary_thickness_cells = 16;
	run_sparse_hierarchy_lookup_contract(descriptor);

	wt::WtAsyncStorageService storage({
		32, 32, wt::kWtMaximumContainerSize, 2
	});
	const auto cold_open_started = std::chrono::steady_clock::now();
	check(storage.open_procedural(descriptor) == wt::WtAsyncStorageStatus::Ok,
		"large procedural storage open failed");
	const auto cold_open_finished = std::chrono::steady_clock::now();
	check(storage.page_count() == 299520 &&
		storage.overlay_page_count() == 0,
		"large procedural storage accounting failed");
	wt::WtEditJournalStore journal;
	check(journal.open(first_journal_root / "world.wtedit", 90001, 0) ==
		wt::WtEditJournalStoreStatus::Ok,
		"large procedural journal open failed");
	const wt::WtGridPoint first_point { 1024, 64, 1024 };
	const wt::WtGridPoint boundary_point { 0, -128, 0 };
	wt::WtAuthoritativeSample sample;
	const auto first_edit_started = std::chrono::steady_clock::now();
	wt::WtEditTransaction first_transaction = sparse_edit(
		90001, 0, 1, 71, first_point, 10.0F
	);
	wt::WtEditCommand boundary_command = first_transaction.commands.front();
	boundary_command.command_id = id(72);
	boundary_command.sequence = 1;
	boundary_command.box = {
		(boundary_point.x - 2) * wt::kWtEditCoordinateScale,
		(boundary_point.y - 2) * wt::kWtEditCoordinateScale,
		(boundary_point.z - 2) * wt::kWtEditCoordinateScale,
		(boundary_point.x + 2) * wt::kWtEditCoordinateScale,
		(boundary_point.y + 2) * wt::kWtEditCoordinateScale,
		(boundary_point.z + 2) * wt::kWtEditCoordinateScale,
	};
	check(wt::wt_edit_box_bounds(boundary_command.box, boundary_command.bounds),
		"large procedural boundary edit bounds failed");
	first_transaction.commands.push_back(boundary_command);
	check(journal.append(first_transaction) == wt::WtEditJournalStoreStatus::Ok,
		"large procedural first edit append failed");
	const auto first_edit_finished = std::chrono::steady_clock::now();
	const auto first_invalidated_query_started = std::chrono::steady_clock::now();
	check(wt::wt_query_authoritative_sample(
		first_point, 0, storage, journal.journal(), 0, sample
	) == wt::WtAuthoritativeSampleQueryStatus::Ok &&
		sample.sample.density == 10.0F && sample.world_revision == 1,
		"large procedural edited query failed");
	check(wt::wt_query_authoritative_sample(
		boundary_point, 0, storage, journal.journal(), 0, sample
	) == wt::WtAuthoritativeSampleQueryStatus::Ok &&
		sample.sample.density < 0.0F &&
		sample.sample.material == wt::kWtProceduralBedrockMaterial &&
		sample.world_revision == 1,
		"large procedural protected boundary edit was not clipped");
	const auto first_invalidated_query_finished = std::chrono::steady_clock::now();
	wt::WtWorldSnapshotStoreResult result;
	const auto first_compaction_started = std::chrono::steady_clock::now();
	const wt::WtWorldSnapshotStoreStatus first_compaction_status =
		wt::wt_write_compacted_world_snapshot(
		storage, journal.journal(), first_snapshot, 90002, result
	);
	check(first_compaction_status == wt::WtWorldSnapshotStoreStatus::Ok &&
		result.source_revision == 90002 && result.world_revision == 1 &&
		result.page_count > 0 &&
		result.page_count < wt::kWtMaximumProceduralOverlayPageCount,
		"large procedural first compaction failed");
	const auto first_compaction_finished = std::chrono::steady_clock::now();
	if (first_compaction_status != wt::WtWorldSnapshotStoreStatus::Ok) {
		std::fprintf(
			stderr,
			"SPARSE_FIRST_COMPACTION_STATUS %s\n",
			wt::wt_world_snapshot_store_status_message(first_compaction_status)
		);
		storage.close();
		journal.close();
		return;
	}
	storage.close();
	journal.close();

	wt::WtAsyncStorageService reopened({
		32, 32, wt::kWtMaximumContainerSize, 2
	});
	const auto first_reopen_started = std::chrono::steady_clock::now();
	check(reopened.open_procedural_snapshot(first_snapshot) ==
		wt::WtAsyncStorageStatus::Ok &&
		reopened.page_count() == 299520 &&
		reopened.overlay_page_count() == result.page_count,
		"large procedural first snapshot reopen failed");
	const auto first_reopen_finished = std::chrono::steady_clock::now();
	wt::WtEditJournalStore second_journal;
	check(second_journal.open(
		second_journal_root / "world.wtedit", 90002, 1
	) == wt::WtEditJournalStoreStatus::Ok,
		"large procedural second journal open failed");
	check(wt::wt_query_authoritative_sample(
		first_point, 0, reopened, second_journal.journal(), 1, sample
	) == wt::WtAuthoritativeSampleQueryStatus::Ok &&
		sample.sample.density == 10.0F && sample.source_revision == 90002,
		"large procedural first snapshot replay failed");
	const wt::WtGridPoint second_point { 1536, 48, 1536 };
	const auto second_edit_started = std::chrono::steady_clock::now();
	check(second_journal.append(sparse_edit(
		90002, 1, 2, 91, second_point, -10.0F
	)) == wt::WtEditJournalStoreStatus::Ok,
		"large procedural second edit append failed");
	const auto second_edit_finished = std::chrono::steady_clock::now();
	const auto second_compaction_started = std::chrono::steady_clock::now();
	const wt::WtWorldSnapshotStoreStatus second_compaction_status =
		wt::wt_write_compacted_world_snapshot(
		reopened, second_journal.journal(), second_snapshot, 90003, result
	);
	check(second_compaction_status == wt::WtWorldSnapshotStoreStatus::Ok &&
		result.source_revision == 90003 && result.world_revision == 2,
		"large procedural second compaction failed");
	const auto second_compaction_finished = std::chrono::steady_clock::now();
	if (second_compaction_status != wt::WtWorldSnapshotStoreStatus::Ok) {
		std::fprintf(
			stderr,
			"SPARSE_SECOND_COMPACTION_STATUS %s\n",
			wt::wt_world_snapshot_store_status_message(second_compaction_status)
		);
		reopened.close();
		second_journal.close();
		return;
	}
	reopened.close();
	second_journal.close();

	wt::WtAsyncStorageService final_storage({
		32, 32, wt::kWtMaximumContainerSize, 2
	});
	const auto final_reopen_started = std::chrono::steady_clock::now();
	check(final_storage.open_procedural_snapshot(second_snapshot) ==
		wt::WtAsyncStorageStatus::Ok,
		"large procedural final snapshot reopen failed");
	const auto final_reopen_finished = std::chrono::steady_clock::now();
	wt::WtEditJournalStore final_journal;
	check(final_journal.open(
		final_journal_root / "world.wtedit", 90003, 2
	) == wt::WtEditJournalStoreStatus::Ok,
		"large procedural final journal open failed");
	check(wt::wt_query_authoritative_sample(
		first_point, 0, final_storage, final_journal.journal(), 2, sample
	) == wt::WtAuthoritativeSampleQueryStatus::Ok &&
		sample.sample.density == 10.0F,
		"large procedural retained first overlay failed");
	check(wt::wt_query_authoritative_sample(
		second_point, 0, final_storage, final_journal.journal(), 2, sample
	) == wt::WtAuthoritativeSampleQueryStatus::Ok &&
		sample.sample.density == -10.0F,
		"large procedural retained second overlay failed");
	check(wt::wt_query_authoritative_sample(
		boundary_point, 0, final_storage, final_journal.journal(), 2, sample
	) == wt::WtAuthoritativeSampleQueryStatus::Ok &&
		sample.sample.density < 0.0F &&
		sample.sample.material == wt::kWtProceduralBedrockMaterial,
		"large procedural compacted boundary protection failed");
	wt::WtWorldSnapshotStoreResult migrated_result;
	const auto migration_started = std::chrono::steady_clock::now();
	const wt::WtWorldSnapshotStoreStatus migration_status =
		wt::wt_write_migrated_world_snapshot(
			final_storage,
			final_journal.journal(),
			migrated_snapshot,
			migrated_result
		);
	const auto migration_finished = std::chrono::steady_clock::now();
	check(migration_status == wt::WtWorldSnapshotStoreStatus::Ok &&
		migrated_result.page_count == final_storage.overlay_page_count() &&
		migrated_result.source_revision == 90003 &&
		migrated_result.world_revision == 2,
		"large procedural sparse migration failed");
	wt::WtAsyncStorageService migrated_storage({
		32, 32, wt::kWtMaximumContainerSize, 2
	});
	const auto migrated_reopen_started = std::chrono::steady_clock::now();
	check(migrated_storage.open_procedural_snapshot(migrated_snapshot) ==
		wt::WtAsyncStorageStatus::Ok &&
		migrated_storage.page_count() == final_storage.page_count() &&
		migrated_storage.overlay_page_count() == final_storage.overlay_page_count(),
		"large procedural migrated snapshot reopen failed");
	const auto migrated_reopen_finished = std::chrono::steady_clock::now();
	wt::WtEditJournalStore migrated_journal;
	check(migrated_journal.open(
		migrated_journal_root / "world.wtedit", 90003, 2
	) == wt::WtEditJournalStoreStatus::Ok,
		"large procedural migrated journal open failed");
	check(wt::wt_query_authoritative_sample(
		first_point, 0, migrated_storage, migrated_journal.journal(), 2, sample
	) == wt::WtAuthoritativeSampleQueryStatus::Ok &&
		sample.sample.density == 10.0F,
		"large procedural migrated first overlay failed");
	check(wt::wt_query_authoritative_sample(
		second_point, 0, migrated_storage, migrated_journal.journal(), 2, sample
	) == wt::WtAuthoritativeSampleQueryStatus::Ok &&
		sample.sample.density == -10.0F,
		"large procedural migrated second overlay failed");
	check(wt::wt_query_authoritative_sample(
		boundary_point, 0, migrated_storage, migrated_journal.journal(), 2, sample
	) == wt::WtAuthoritativeSampleQueryStatus::Ok &&
		sample.sample.density < 0.0F &&
		sample.sample.material == wt::kWtProceduralBedrockMaterial,
		"large procedural migrated boundary protection failed");

	const std::filesystem::path corrupt = root / "snapshot-corrupt";
	std::filesystem::create_directories(corrupt);
	std::vector<std::uint8_t> descriptor_bytes = read_file(
		second_snapshot / "world.wtproc"
	);
	check(!descriptor_bytes.empty(), "procedural descriptor evidence missing");
	if (!descriptor_bytes.empty()) descriptor_bytes.back() ^= 0x80U;
	std::error_code copy_error;
	check(write_file(corrupt / "world.wtproc", descriptor_bytes) &&
		std::filesystem::copy_file(
			second_snapshot / "world.wtworld",
			corrupt / "world.wtworld",
			copy_error
		), "corrupt procedural snapshot fixture failed");
	wt::WtAsyncStorageService corrupt_storage({
		8, 8, wt::kWtMaximumContainerSize, 1
	});
	check(corrupt_storage.open_procedural_snapshot(corrupt) ==
		wt::WtAsyncStorageStatus::ManifestFailure &&
		!corrupt_storage.is_open(),
		"corrupt procedural descriptor did not fail closed");
	const std::filesystem::path missing_manifest = root / "snapshot-missing-manifest";
	std::filesystem::create_directories(missing_manifest);
	check(std::filesystem::copy_file(
			second_snapshot / "world.wtproc",
			missing_manifest / "world.wtproc",
			copy_error
		), "missing-manifest fixture write failed");
	wt::WtAsyncStorageService missing_manifest_storage({
		8, 8, wt::kWtMaximumContainerSize, 1
	});
	const wt::WtAsyncStorageStatus missing_manifest_status =
		missing_manifest_storage.open_procedural_snapshot(missing_manifest);
	check(missing_manifest_status != wt::WtAsyncStorageStatus::Ok &&
		!missing_manifest_storage.is_open(),
		"missing procedural overlay manifest did not fail closed");
	const std::filesystem::path missing_objects = root / "snapshot-missing-objects";
	std::filesystem::create_directories(missing_objects);
	copy_error.clear();
	check(std::filesystem::copy_file(
			second_snapshot / "world.wtproc",
			missing_objects / "world.wtproc",
			copy_error
		) && (copy_error.clear(), std::filesystem::copy_file(
			second_snapshot / "world.wtworld",
			missing_objects / "world.wtworld",
			copy_error
		)), "missing-object fixture write failed");
	wt::WtAsyncStorageService missing_object_storage({
		8, 8, wt::kWtMaximumContainerSize, 1
	});
	check(missing_object_storage.open_procedural_snapshot(missing_objects) ==
		wt::WtAsyncStorageStatus::Ok,
		"missing-object sparse descriptor did not open for demand test");
	const wt::WtChunkKey first_overlay_key {
		static_cast<std::int32_t>(first_point.x / wt::kWtChunkCellsPerAxis),
		static_cast<std::int32_t>(first_point.y / wt::kWtChunkCellsPerAxis),
		static_cast<std::int32_t>(first_point.z / wt::kWtChunkCellsPerAxis),
		0,
	};
	std::shared_ptr<const std::vector<std::uint8_t>> missing_page;
	check(missing_object_storage.has_overlay_page(first_overlay_key) &&
		missing_object_storage.load_page_now(first_overlay_key, missing_page) !=
			wt::WtPageLoadStatus::Ok && !missing_page,
		"missing sparse overlay object did not fail closed on demand");
	missing_object_storage.close();

	std::filesystem::create_directories(interrupted_snapshot.string() + ".tmp");
	check(write_file(
		interrupted_snapshot.string() + ".tmp/interrupted.marker", { 1, 2, 3 }
	), "sparse interrupted-publication marker write failed");
	wt::WtWorldSnapshotStoreResult rejection_result;
	check(wt::wt_write_migrated_world_snapshot(
			final_storage,
			final_journal.journal(),
			interrupted_snapshot,
			rejection_result
		) == wt::WtWorldSnapshotStoreStatus::IoFailure &&
		!std::filesystem::exists(interrupted_snapshot) &&
		std::filesystem::exists(
			interrupted_snapshot.string() + ".tmp/interrupted.marker"
		), "sparse interrupted publication did not fail closed");

	wt::WtAsyncStorageService capacity_storage({
		8, 8, wt::kWtMaximumContainerSize, 1
	});
	check(capacity_storage.open_procedural(descriptor) ==
		wt::WtAsyncStorageStatus::Ok,
		"sparse capacity source open failed");
	wt::WtEditJournalStore capacity_journal;
	check(capacity_journal.open(
		capacity_journal_root / "world.wtedit", 90001, 0
	) == wt::WtEditJournalStoreStatus::Ok,
		"sparse capacity journal open failed");
	wt::WtEditTransaction capacity_transaction = sparse_edit(
		90001, 0, 1, 111, { 1024, 0, 1024 }, 10.0F
	);
	wt::WtEditCommand &capacity_command = capacity_transaction.commands.front();
	capacity_command.box = {
		-2 * wt::kWtEditCoordinateScale,
		-130 * wt::kWtEditCoordinateScale,
		-2 * wt::kWtEditCoordinateScale,
		2050 * wt::kWtEditCoordinateScale,
		130 * wt::kWtEditCoordinateScale,
		2050 * wt::kWtEditCoordinateScale,
	};
	check(wt::wt_edit_box_bounds(capacity_command.box, capacity_command.bounds) &&
		capacity_journal.append(capacity_transaction) ==
			wt::WtEditJournalStoreStatus::Ok,
		"sparse capacity transaction failed");
	check(wt::wt_write_compacted_world_snapshot(
			capacity_storage,
			capacity_journal.journal(),
			capacity_snapshot,
			90002,
			rejection_result
		) == wt::WtWorldSnapshotStoreStatus::CapacityExceeded &&
		!std::filesystem::exists(capacity_snapshot) &&
		!std::filesystem::exists(capacity_snapshot.string() + ".tmp"),
		"sparse overlay capacity exhaustion did not fail closed");
	capacity_storage.close();
	capacity_journal.close();
	const auto compaction_ns =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			first_compaction_finished - first_compaction_started
		).count();
	std::printf(
		"SPARSE_SNAPSHOT_EVIDENCE declared_pages=%zu overlay_pages=%zu "
		"overlay_index_bytes=%zu hierarchy_index_bytes=%zu "
		"cold_open_ns=%lld first_edit_append_ns=%lld "
		"edit_invalidation_query_ns=%lld first_compaction_ns=%lld "
		"first_reopen_ns=%lld second_edit_append_ns=%lld "
		"second_compaction_ns=%lld final_reopen_ns=%lld migration_ns=%lld "
		"migrated_reopen_ns=%lld\n",
		final_storage.page_count(),
		final_storage.overlay_page_count(),
		final_storage.overlay_index_bytes(),
		final_storage.page_hierarchy().metrics().estimated_index_bytes,
		static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			cold_open_finished - cold_open_started
		).count()),
		static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			first_edit_finished - first_edit_started
		).count()),
		static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			first_invalidated_query_finished - first_invalidated_query_started
		).count()),
		static_cast<long long>(compaction_ns),
		static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			first_reopen_finished - first_reopen_started
		).count()),
		static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			second_edit_finished - second_edit_started
		).count()),
		static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			second_compaction_finished - second_compaction_started
		).count()),
		static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			final_reopen_finished - final_reopen_started
		).count()),
		static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			migration_finished - migration_started
		).count()),
		static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			migrated_reopen_finished - migrated_reopen_started
		).count())
	);
	migrated_storage.close();
	migrated_journal.close();
	final_storage.close();
	final_journal.close();
}

} // namespace

int main() {
	FixtureRoot fixture;
	const std::filesystem::path source_root = fixture.path / "source";
	const std::filesystem::path compacted_root = fixture.path / "compacted";
	const std::filesystem::path migrated_root = fixture.path / "migrated";
	std::filesystem::create_directories(fixture.path);
	run_sparse_procedural_snapshot_contract(fixture.path / "sparse-procedural");
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
		"migrations=1 rejection_cases=6 sparse_rejection_cases=8\n"
	);
	return 0;
}
