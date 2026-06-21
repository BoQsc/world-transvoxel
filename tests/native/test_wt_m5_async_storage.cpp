#include "bake/wt_chunk_baker.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_hash256.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wt = world_transvoxel;

namespace {

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

wt::WtHash256 hash_text(const char *text) {
	const std::string value(text);
	return wt::wt_sha256(
		reinterpret_cast<const std::uint8_t *>(value.data()),
		value.size()
	);
}

wt::WtDependencyEntry dependency(
	wt::WtDependencyKind kind,
	const char *label,
	const char *version,
	const char *identity
) {
	return { kind, label, version, hash_text(identity) };
}

class FixtureSource final : public wt::WtChunkSampleSource {
public:
	bool sample(
		const wt::WtGridPoint &point,
		wt::WtScalarSample &output
	) const noexcept override {
		output.density =
			static_cast<float>(point.x - point.y + point.z) * 0.03125F;
		output.material = static_cast<std::uint16_t>(
			(static_cast<std::uint64_t>(point.x) * 3U) ^
			(static_cast<std::uint64_t>(point.y) * 5U) ^
			(static_cast<std::uint64_t>(point.z) * 7U)
		);
		return true;
	}
};

bool write_file(
	const std::filesystem::path &path,
	const std::vector<std::uint8_t> &bytes
) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) {
		return false;
	}
	if (!bytes.empty()) {
		output.write(
			reinterpret_cast<const char *>(bytes.data()),
			static_cast<std::streamsize>(bytes.size())
		);
	}
	return static_cast<bool>(output);
}

struct StorageFixture {
	std::filesystem::path root;
	std::filesystem::path world_path;
	std::filesystem::path corrupt_world_path;
	std::vector<wt::WtBakedChunkPage> pages;

	StorageFixture() = default;
	StorageFixture(const StorageFixture &) = delete;
	StorageFixture &operator=(const StorageFixture &) = delete;
	StorageFixture(StorageFixture &&other) noexcept :
			root(std::move(other.root)),
			world_path(std::move(other.world_path)),
			corrupt_world_path(std::move(other.corrupt_world_path)),
			pages(std::move(other.pages)) {
		other.root.clear();
	}

	~StorageFixture() {
		if (root.empty()) {
			return;
		}
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}
};

StorageFixture make_fixture() {
	StorageFixture fixture;
	fixture.root = std::filesystem::temp_directory_path() /
		("wt_m5_async_storage_" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()
		));
	std::error_code error;
	check(
		std::filesystem::create_directories(fixture.root, error) && !error,
		"fixture directory creation failed"
	);

	const std::vector<wt::WtChunkKey> keys = {
		{ 0, 0, 0, 0 },
		{ 1, 0, 0, 0 },
		{ 2, 0, 0, 0 },
		{ 3, 0, 0, 0 },
		{ 4, 0, 0, 0 },
	};
	const FixtureSource source;
	wt::WtChunkBaker baker(keys.size());
	check(
		baker.bake(keys, 9001, source, fixture.pages) ==
			wt::WtChunkBakeStatus::Ok,
		"fixture page bake failed"
	);

	wt::WtWorldManifest manifest;
	manifest.source_revision = 9001;
	manifest.world_revision = 41;
	manifest.configuration_hash = hash_text("m5-storage-configuration");
	manifest.dependencies = {
		dependency(wt::WtDependencyKind::SourceAsset,
			"fixture-density", "", "fixture-density-bytes"),
		dependency(wt::WtDependencyKind::Generator,
			"world-transvoxel-baker", "0.5.0-m4", "m5-generator"),
		dependency(wt::WtDependencyKind::Configuration,
			"m5-storage", "1", "m5-storage-configuration"),
		dependency(wt::WtDependencyKind::Backend,
			"transvoxel-mit", "fixture", "m5-backend"),
		dependency(wt::WtDependencyKind::Godot,
			"godot", "4.6.3", "m5-godot"),
		dependency(wt::WtDependencyKind::GodotCpp,
			"godot-cpp", "e83fd090", "m5-godot-cpp"),
		dependency(wt::WtDependencyKind::Toolchain,
			"zig", "0.16.0", "m5-zig"),
	};
	for (const wt::WtBakedChunkPage &page : fixture.pages) {
		manifest.pages.push_back({
			page.key,
			page.bytes.size(),
			page.content_hash,
		});
	}
	manifest.pages[3].byte_size = fixture.pages[0].bytes.size();
	manifest.pages[3].content_hash = fixture.pages[0].content_hash;
	std::vector<std::uint8_t> world_bytes;
	check(
		wt::wt_write_world_manifest(manifest, world_bytes) ==
			wt::WtWorldManifestStatus::Ok,
		"fixture manifest write failed"
	);
	fixture.world_path = fixture.root / "world.wtworld";
	check(write_file(fixture.world_path, world_bytes), "world file write failed");

	check(
		write_file(
			wt::wt_page_object_path(
				fixture.root,
				fixture.pages[0].content_hash
			),
			fixture.pages[0].bytes
		),
		"valid page write failed"
	);
	std::vector<std::uint8_t> short_page = fixture.pages[1].bytes;
	short_page.pop_back();
	check(
		write_file(
			wt::wt_page_object_path(
				fixture.root,
				fixture.pages[1].content_hash
			),
			short_page
		),
		"short page write failed"
	);
	std::vector<std::uint8_t> corrupt_page = fixture.pages[2].bytes;
	corrupt_page.back() ^= 0x80U;
	check(
		write_file(
			wt::wt_page_object_path(
				fixture.root,
				fixture.pages[2].content_hash
			),
			corrupt_page
		),
		"corrupt page write failed"
	);
	fixture.corrupt_world_path = fixture.root / "corrupt.wtworld";
	check(
		write_file(fixture.corrupt_world_path, { 1, 2, 3, 4 }),
		"corrupt world write failed"
	);
	return fixture;
}

bool wait_for_started(
	const wt::WtAsyncStorageService &service,
	std::uint64_t count
) {
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (std::chrono::steady_clock::now() < deadline) {
		if (service.get_metrics().started_requests >= count) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

bool wait_for_completion_count(
	const wt::WtAsyncStorageService &service,
	std::size_t count
) {
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (std::chrono::steady_clock::now() < deadline) {
		if (service.queued_completion_count() >= count) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

wt::WtPageLoadCompletion wait_completion(
	wt::WtAsyncStorageService &service,
	const wt::WtChunkKey &key,
	std::uint64_t generation,
	wt::WtPageLoadStatus status
) {
	wt::WtPageLoadCompletion completion;
	check(
		service.wait_pop_completion(
			completion,
			std::chrono::seconds(3)
		),
		"timed out waiting for storage completion"
	);
	check(completion.key == key, "completion key mismatch");
	check(
		completion.generation.value == generation,
		"completion generation mismatch"
	);
	check(completion.status == status, "completion status mismatch");
	check(
		(status == wt::WtPageLoadStatus::Ok) ==
			static_cast<bool>(completion.page_bytes),
		"completion payload ownership mismatch"
	);
	return completion;
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
	for (std::size_t index = 0; index < 8; ++index) {
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
	}
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void test_open_failures(const StorageFixture &fixture) {
	wt::WtAsyncStorageService invalid({ 0, 1, wt::kWtMaximumContainerSize });
	check(
		invalid.open(fixture.world_path, fixture.root) ==
			wt::WtAsyncStorageStatus::InvalidConfiguration,
		"zero request capacity was accepted"
	);
	wt::WtAsyncStorageService small({
		1,
		1,
		fixture.pages[0].bytes.size() - 1,
	});
	check(
		small.open(fixture.world_path, fixture.root) ==
			wt::WtAsyncStorageStatus::PageSizeLimitExceeded,
		"manifest exceeding page memory limit was accepted"
	);
	wt::WtAsyncStorageService corrupt({ 1, 1, wt::kWtMaximumContainerSize });
	check(
		corrupt.open(fixture.corrupt_world_path, fixture.root) ==
			wt::WtAsyncStorageStatus::ManifestFailure,
		"corrupt manifest was accepted"
	);
	check(
		corrupt.open(fixture.root / "missing.wtworld", fixture.root) ==
			wt::WtAsyncStorageStatus::InvalidPath,
		"missing manifest path was accepted"
	);
	const std::string object_name = wt::wt_page_object_path(
		fixture.root,
		fixture.pages[0].content_hash
	).filename().string();
	check(
		object_name.size() == 72 &&
			object_name.substr(64) == ".wtchunk",
		"content-addressed object name is not canonical"
	);
}

void test_async_service(
	const StorageFixture &fixture,
	std::vector<std::uint8_t> &evidence
) {
	wt::WtAsyncStorageService service({
		2,
		1,
		wt::kWtMaximumContainerSize,
	});
	check(
		service.open(fixture.world_path, fixture.root) ==
			wt::WtAsyncStorageStatus::Ok,
		"storage service open failed"
	);
	check(service.is_open(), "open service reports closed");
	check(service.source_revision() == 9001, "source revision mismatch");
	check(
		service.open(fixture.world_path, fixture.root) ==
			wt::WtAsyncStorageStatus::AlreadyOpen,
		"double open was accepted"
	);

	check(
		service.request_page(fixture.pages[0].key, { 11 }, 1) ==
			wt::WtAsyncStorageStatus::Ok,
		"valid request was rejected"
	);
	check(
		wait_for_completion_count(service, 1),
		"first completion was not produced asynchronously"
	);
	check(
		service.request_page(fixture.pages[1].key, { 12 }, 2) ==
			wt::WtAsyncStorageStatus::Ok,
		"second request was rejected"
	);
	check(wait_for_started(service, 2), "second request did not start");
	check(
		service.request_page(fixture.pages[2].key, { 13 }, 3) ==
			wt::WtAsyncStorageStatus::Ok,
		"bounded queued request was rejected"
	);
	check(
		service.request_page(fixture.pages[3].key, { 14 }, 10) ==
			wt::WtAsyncStorageStatus::Ok,
		"higher-priority queued request was rejected"
	);
	check(
		service.request_page(fixture.pages[2].key, { 13 }, 3) ==
			wt::WtAsyncStorageStatus::AlreadyPending,
		"duplicate pending request was accepted"
	);
	check(
		service.request_page(fixture.pages[4].key, { 15 }, 4) ==
			wt::WtAsyncStorageStatus::RequestQueueFull,
		"request queue overflow was accepted"
	);

	const wt::WtPageLoadCompletion valid = wait_completion(
		service,
		fixture.pages[0].key,
		11,
		wt::WtPageLoadStatus::Ok
	);
	check(
		valid.page_bytes && *valid.page_bytes == fixture.pages[0].bytes,
		"valid page bytes changed during asynchronous load"
	);
	const wt::WtPageLoadCompletion short_page = wait_completion(
		service,
		fixture.pages[1].key,
		12,
		wt::WtPageLoadStatus::SizeMismatch
	);
	const wt::WtPageLoadCompletion metadata = wait_completion(
		service,
		fixture.pages[3].key,
		14,
		wt::WtPageLoadStatus::MetadataMismatch
	);
	const wt::WtPageLoadCompletion corrupt_page = wait_completion(
		service,
		fixture.pages[2].key,
		13,
		wt::WtPageLoadStatus::HashMismatch
	);
	check(
		service.request_page(fixture.pages[4].key, { 15 }, 5) ==
			wt::WtAsyncStorageStatus::Ok,
		"missing object request was rejected"
	);
	const wt::WtPageLoadCompletion missing = wait_completion(
		service,
		fixture.pages[4].key,
		15,
		wt::WtPageLoadStatus::ObjectMissing
	);
	check(
		service.request_page({ 99, 0, 0, 0 }, { 16 }, 0) ==
			wt::WtAsyncStorageStatus::PageNotFound,
		"unindexed page request was queued"
	);
	check(
		service.request_page({ 0, 0, 0, 21 }, { 17 }, 0) ==
			wt::WtAsyncStorageStatus::InvalidKey &&
		service.request_page(fixture.pages[0].key, { 0 }, 0) ==
			wt::WtAsyncStorageStatus::InvalidKey,
		"invalid request identity was accepted"
	);

	const wt::WtAsyncStorageMetrics metrics = service.get_metrics();
	check(
		metrics.accepted_requests == 5 &&
			metrics.started_requests == 5 &&
			metrics.completed_requests == 5,
		"request lifecycle metrics mismatch"
	);
	check(
		metrics.successful_pages == 1 &&
			metrics.failed_pages == 4 &&
			metrics.request_queue_rejections == 1 &&
			metrics.duplicate_requests == 1,
		"storage result metrics mismatch"
	);
	check(
		metrics.bytes_read ==
			fixture.pages[0].bytes.size() * 2 +
			fixture.pages[2].bytes.size(),
		"storage byte metrics mismatch"
	);
	const std::uint64_t started_before_idle = metrics.started_requests;
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	check(
		service.get_metrics().started_requests == started_before_idle,
		"idle storage service performed unrequested work"
	);
	check(
		service.queued_request_count() == 0 &&
			service.queued_completion_count() == 0,
		"storage queues did not drain"
	);

	evidence = fixture.pages[0].bytes;
	for (const wt::WtPageLoadCompletion *completion : {
			&valid, &short_page, &metadata, &corrupt_page, &missing
		}) {
		evidence.push_back(static_cast<std::uint8_t>(completion->status));
		append_u64(evidence, completion->generation.value);
	}
	append_u64(evidence, metrics.accepted_requests);
	append_u64(evidence, metrics.bytes_read);
	service.close();
	check(!service.is_open(), "closed service reports open");
	check(
		service.request_page(fixture.pages[0].key, { 18 }, 0) ==
			wt::WtAsyncStorageStatus::NotOpen,
		"closed service accepted a request"
	);
}

void test_shutdown_accounting(const StorageFixture &fixture) {
	wt::WtAsyncStorageService service({
		1,
		1,
		wt::kWtMaximumContainerSize,
	});
	check(
		service.open(fixture.world_path, fixture.root) ==
			wt::WtAsyncStorageStatus::Ok,
		"shutdown fixture open failed"
	);
	check(
		service.request_page(fixture.pages[0].key, { 21 }, 1) ==
			wt::WtAsyncStorageStatus::Ok &&
			wait_for_completion_count(service, 1),
		"shutdown completion fixture failed"
	);
	check(
		service.request_page(fixture.pages[1].key, { 22 }, 2) ==
			wt::WtAsyncStorageStatus::Ok &&
			wait_for_started(service, 2),
		"shutdown in-flight fixture failed"
	);
	check(
		service.request_page(fixture.pages[2].key, { 23 }, 3) ==
			wt::WtAsyncStorageStatus::Ok,
		"shutdown queued fixture failed"
	);
	service.close();
	const wt::WtAsyncStorageMetrics metrics = service.get_metrics();
	check(
		metrics.cancelled_requests == 2 &&
			metrics.discarded_completions == 1,
		"shutdown accounting mismatch"
	);
}

} // namespace

int main() {
	StorageFixture fixture = make_fixture();
	if (fixture.pages.size() != 5) {
		std::fprintf(stderr, "M5_ASYNC_STORAGE_FAIL fixture_pages=%zu\n",
			fixture.pages.size());
		return 1;
	}
	test_open_failures(fixture);
	std::vector<std::uint8_t> evidence;
	test_async_service(fixture, evidence);
	test_shutdown_accounting(fixture);
	if (failure_count != 0) {
		std::fprintf(stderr, "M5_ASYNC_STORAGE_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M5_ASYNC_STORAGE_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"M5_ASYNC_STORAGE_PASS requests=5 successes=1 failures=4 "
		"queue_rejections=1\n"
	);
	return 0;
}
