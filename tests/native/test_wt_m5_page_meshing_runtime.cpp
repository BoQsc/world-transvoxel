#include "backend/wt_transvoxel_mit_backend.h"
#include "bake/wt_chunk_baker.h"
#include "services/wt_page_meshing_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_chunk_page_sample_source.h"
#include "storage/wt_hash256.h"
#include "storage/wt_storage_page_cache.h"
#include "wt_m2_mesh_test_support.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wt = world_transvoxel;
using namespace world_transvoxel_test;

namespace {

constexpr std::uint64_t kSourceRevision = 7101;
constexpr std::uint64_t kWorldRevision = 91;
constexpr std::size_t kDependencyCount = 13;

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

class SphereSource final : public wt::WtChunkSampleSource {
public:
	bool sample(
		const wt::WtGridPoint &point,
		wt::WtScalarSample &output
	) const noexcept override {
		const double dx = static_cast<double>(point.x) - 32.0;
		const double dy = static_cast<double>(point.y) - 32.0;
		const double dz = static_cast<double>(point.z) - 32.0;
		output.density = static_cast<float>(
			std::sqrt(dx * dx + dy * dy + dz * dz) - 8.25
		);
		output.material = 7;
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

std::vector<wt::WtDependencyEntry> fixture_dependencies() {
	return {
		dependency(wt::WtDependencyKind::SourceAsset,
			"runtime-density", "", "runtime-density-bytes"),
		dependency(wt::WtDependencyKind::Generator,
			"world-transvoxel-baker", "0.5.0-m4", "runtime-generator"),
		dependency(wt::WtDependencyKind::Configuration,
			"m5-page-runtime", "1", "runtime-configuration"),
		dependency(wt::WtDependencyKind::Backend,
			"transvoxel-mit", "fixture", "runtime-backend"),
		dependency(wt::WtDependencyKind::Godot,
			"godot", "4.6.3", "runtime-godot"),
		dependency(wt::WtDependencyKind::GodotCpp,
			"godot-cpp", "e83fd090", "runtime-godot-cpp"),
		dependency(wt::WtDependencyKind::Toolchain,
			"zig", "0.16.0", "runtime-zig"),
	};
}

struct RuntimeFixture {
	std::filesystem::path root;
	std::filesystem::path world_path;
	std::filesystem::path incomplete_world_path;
	wt::WtChunkKey coarse_key = { 0, 0, 0, 1 };
	std::uint8_t transition_mask = 0;
	std::vector<wt::WtChunkKey> support_keys;
	std::vector<wt::WtBakedChunkPage> pages;

	RuntimeFixture() = default;
	RuntimeFixture(const RuntimeFixture &) = delete;
	RuntimeFixture &operator=(const RuntimeFixture &) = delete;
	RuntimeFixture(RuntimeFixture &&other) noexcept :
			root(std::move(other.root)),
			world_path(std::move(other.world_path)),
			incomplete_world_path(std::move(other.incomplete_world_path)),
			coarse_key(other.coarse_key),
			transition_mask(other.transition_mask),
			support_keys(std::move(other.support_keys)),
			pages(std::move(other.pages)) {
		other.root.clear();
	}

	~RuntimeFixture() {
		if (root.empty()) {
			return;
		}
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}
};

bool write_manifest(
	const RuntimeFixture &fixture,
	const std::filesystem::path &path,
	const wt::WtChunkKey *omitted_key
) {
	wt::WtWorldManifest manifest;
	manifest.source_revision = kSourceRevision;
	manifest.world_revision = kWorldRevision;
	manifest.configuration_hash = hash_text("runtime-configuration");
	manifest.dependencies = fixture_dependencies();
	for (const wt::WtBakedChunkPage &page : fixture.pages) {
		if (omitted_key != nullptr && page.key == *omitted_key) {
			continue;
		}
		manifest.pages.push_back({
			page.key,
			page.bytes.size(),
			page.content_hash,
		});
	}
	std::vector<std::uint8_t> bytes;
	return wt::wt_write_world_manifest(manifest, bytes) ==
			wt::WtWorldManifestStatus::Ok &&
		write_file(path, bytes);
}

RuntimeFixture make_fixture() {
	RuntimeFixture fixture;
	fixture.root = std::filesystem::temp_directory_path() /
		("wt_m5_page_runtime_" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()
		));
	std::error_code error;
	check(
		std::filesystem::create_directories(fixture.root, error) && !error,
		"runtime fixture directory creation failed"
	);

	fixture.transition_mask = static_cast<std::uint8_t>(
		wt::wt_face_bit(wt::WtChunkFace::PositiveX) |
		wt::wt_face_bit(wt::WtChunkFace::PositiveY) |
		wt::wt_face_bit(wt::WtChunkFace::PositiveZ)
	);
	std::set<wt::WtChunkKey> unique_support;
	for (wt::WtChunkFace face : {
			wt::WtChunkFace::PositiveX,
			wt::WtChunkFace::PositiveY,
			wt::WtChunkFace::PositiveZ,
		}) {
		std::array<wt::WtChunkKey, wt::kWtTransitionSupportPagesPerFace>
			support{};
		check(
			wt::wt_transition_support_page_keys(
				fixture.coarse_key,
				face,
				support
			),
			"runtime support key generation failed"
		);
		unique_support.insert(support.begin(), support.end());
	}
	fixture.support_keys.assign(unique_support.begin(), unique_support.end());
	std::vector<wt::WtChunkKey> keys = { fixture.coarse_key };
	keys.insert(keys.end(), fixture.support_keys.begin(), fixture.support_keys.end());
	const SphereSource source;
	wt::WtChunkBaker baker(keys.size());
	check(
		baker.bake(keys, kSourceRevision, source, fixture.pages) ==
			wt::WtChunkBakeStatus::Ok,
		"runtime fixture page bake failed"
	);
	for (const wt::WtBakedChunkPage &page : fixture.pages) {
		check(
			write_file(
				wt::wt_page_object_path(fixture.root, page.content_hash),
				page.bytes
			),
			"runtime fixture page write failed"
		);
	}
	fixture.world_path = fixture.root / "world.wtworld";
	fixture.incomplete_world_path = fixture.root / "incomplete.wtworld";
	check(
		write_manifest(fixture, fixture.world_path, nullptr),
		"runtime fixture manifest write failed"
	);
	check(
		!fixture.support_keys.empty() &&
		write_manifest(
			fixture,
			fixture.incomplete_world_path,
			&fixture.support_keys.front()
		),
		"incomplete runtime fixture manifest write failed"
	);
	return fixture;
}

wt::WtChunkJob request_sample_job(
	wt::WtStreamScheduler &scheduler,
	const wt::WtChunkKey &key,
	std::uint64_t world_revision,
	std::int32_t priority
) {
	wt::WtChunkJob job;
	check(
		scheduler.request_chunk_version(
			key,
			kSourceRevision,
			world_revision,
			priority
		) == wt::WtSchedulerStatus::Ok &&
		scheduler.pop_job(job) &&
		job.stage == wt::WtChunkJobStage::Sample,
		"sample job request failed"
	);
	return job;
}

bool wait_completion(
	wt::WtAsyncStorageService &storage,
	wt::WtPageLoadCompletion &completion
) {
	const bool completed = storage.wait_pop_completion(
		completion,
		std::chrono::seconds(3)
	);
	check(completed, "timed out waiting for runtime page");
	return completed;
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

void test_runtime_lifecycle(
	const RuntimeFixture &fixture,
	std::vector<std::uint8_t> &evidence
) {
	wt::WtAsyncStorageService storage({ 32, 32, wt::kWtMaximumContainerSize });
	check(
		storage.open(fixture.world_path, fixture.root) ==
			wt::WtAsyncStorageStatus::Ok,
		"runtime storage open failed"
	);
	wt::WtStoragePageCache cache({
		2,
		wt::kWtMaximumContainerSize,
		2,
		wt::kWtMaximumContainerSize,
	});
	wt::WtStreamScheduler scheduler(8, 8, 1, 1);
	wt::WtPageMeshingRuntimeService runtime(8);
	const wt::WtChunkJob sample = request_sample_job(
		scheduler,
		fixture.coarse_key,
		kWorldRevision,
		7
	);
	check(
		runtime.begin_sample_job(
			sample,
			fixture.transition_mask,
			storage,
			cache,
			scheduler
		) == wt::WtPageMeshingRuntimeStatus::Ok,
		"page runtime rejected sample job"
	);
	const auto loading = runtime.get_records();
	check(
		loading.size() == 1 &&
		loading[0].phase == wt::WtPageMeshingRuntimePhase::Loading &&
		loading[0].dependency_count == kDependencyCount,
		"runtime loading record mismatch"
	);
	check(
		scheduler.submit_completion({
			{ 99, 99, 99, 0 },
			{ 999 },
			wt::WtChunkJobStage::Sample,
			true,
		}) == wt::WtSchedulerStatus::Ok,
		"scheduler backpressure fixture failed"
	);

	std::size_t backpressure_count = 0;
	for (std::size_t index = 0; index < kDependencyCount; ++index) {
		wt::WtPageLoadCompletion completion;
		if (!wait_completion(storage, completion)) {
			break;
		}
		const wt::WtPageMeshingRuntimeStatus status =
			runtime.accept_storage_completion(completion, cache, scheduler);
		if (status == wt::WtPageMeshingRuntimeStatus::SchedulerBackpressure) {
			++backpressure_count;
		} else {
			check(
				status == wt::WtPageMeshingRuntimeStatus::Ok,
				"runtime rejected valid storage completion"
			);
		}
	}
	const auto loaded = runtime.get_records();
	check(
		backpressure_count == 1 && loaded.size() == 1 &&
		loaded[0].phase == wt::WtPageMeshingRuntimePhase::SampleReady &&
		loaded[0].pinned_page_count == kDependencyCount &&
		runtime.pinned_page_count() == kDependencyCount &&
		cache.decoded_entry_count() == 2,
		"runtime pin/backpressure contract mismatch"
	);
	check(
		runtime.reprioritize_owned_chunk(
			fixture.coarse_key,
			sample.generation,
			11
		) == wt::WtPageMeshingRuntimeOwnerStatus::Ok &&
		runtime.reprioritize_owned_chunk(
			fixture.coarse_key,
			{ sample.generation.value + 1 },
			12
		) == wt::WtPageMeshingRuntimeOwnerStatus::StaleGeneration &&
		runtime.get_records()[0].priority == 11,
		"runtime owner repriority/generation guard mismatch"
	);
	check(
		scheduler.apply_completions(1) == 1 &&
		runtime.flush_scheduler_results(scheduler) == 1 &&
		scheduler.apply_completions(1) == 1,
		"sample completion retry failed"
	);

	wt::WtChunkJob mesh_job;
	check(
		scheduler.pop_job(mesh_job) &&
		mesh_job.stage == wt::WtChunkJobStage::Mesh,
		"runtime mesh job was not scheduled"
	);
	const wt::WtChunkMesher mesher(wt::wt_get_transvoxel_mit_backend());
	wt::WtChunkMeshingScratch scratch;
	check(
		runtime.execute_mesh_job(mesh_job, mesher, scratch, scheduler) ==
			wt::WtPageMeshingRuntimeStatus::Ok,
		"page-backed runtime meshing failed"
	);
	const auto meshed = runtime.get_records();
	check(
		meshed.size() == 1 &&
		meshed[0].phase == wt::WtPageMeshingRuntimePhase::Ready &&
		meshed[0].pinned_page_count == 0 &&
		runtime.pinned_page_count() == 0,
		"runtime did not release page pins after meshing"
	);
	check(
		scheduler.apply_completions(1) == 1 &&
		scheduler.find_record(fixture.coarse_key) != nullptr &&
		scheduler.find_record(fixture.coarse_key)->lifecycle ==
			wt::WtChunkLifecycle::Ready,
		"scheduler did not accept runtime mesh completion"
	);
	wt::WtPageMeshCompletion mesh_completion;
	check(
		runtime.pop_mesh_completion(mesh_completion) &&
		mesh_completion.key == fixture.coarse_key &&
		mesh_completion.generation == sample.generation &&
		mesh_completion.mesh != nullptr,
		"runtime mesh completion ownership mismatch"
	);
	std::uint64_t mesh_hash = 14695981039346656037ULL;
	if (mesh_completion.mesh) {
		validate_buffer(
			mesh_completion.mesh->regular,
			"invalid runtime regular mesh"
		);
		std::size_t transition_indices = 0;
		for (std::size_t face = 0; face < 6; ++face) {
			transition_indices +=
				mesh_completion.mesh->transitions[face].indices.size();
		}
		check(
			transition_indices != 0,
			"runtime transition mesh is empty"
		);
		hash_result(mesh_hash, *mesh_completion.mesh);
	}

	std::vector<wt::WtPageMeshingInvalidation> invalidated;
	check(
		runtime.invalidate_dependency(
			fixture.support_keys.front(),
			invalidated
		) == wt::WtPageMeshingRuntimeStatus::Ok &&
		invalidated.size() == 1 &&
		invalidated[0].key == fixture.coarse_key &&
		invalidated[0].generation == sample.generation &&
		runtime.record_count() == 0,
		"support-page invalidation did not retire the coarse generation"
	);
	check(
		scheduler.cancel_chunk(fixture.coarse_key) ==
			wt::WtSchedulerStatus::Ok &&
		scheduler.forget_chunk(fixture.coarse_key) ==
			wt::WtSchedulerStatus::Ok,
		"invalidation scheduler cleanup failed"
	);

	wt::WtStoragePageCache cancellation_cache({
		2,
		wt::kWtMaximumContainerSize,
		2,
		wt::kWtMaximumContainerSize,
	});
	const wt::WtChunkJob cancelled = request_sample_job(
		scheduler,
		fixture.coarse_key,
		kWorldRevision + 1,
		3
	);
	check(
		runtime.begin_sample_job(
			cancelled,
			fixture.transition_mask,
			storage,
			cancellation_cache,
			scheduler
		) == wt::WtPageMeshingRuntimeStatus::Ok &&
		runtime.cancel_owned_generation(
			fixture.coarse_key,
			cancelled.generation
		) == wt::WtPageMeshingRuntimeOwnerStatus::Ok &&
		scheduler.cancel_chunk(fixture.coarse_key) ==
			wt::WtSchedulerStatus::Ok,
		"runtime generation cancellation failed"
	);
	std::size_t stale_count = 0;
	for (std::size_t index = 0; index < kDependencyCount; ++index) {
		wt::WtPageLoadCompletion completion;
		if (!wait_completion(storage, completion)) {
			break;
		}
		stale_count += runtime.accept_storage_completion(
			completion,
			cancellation_cache,
			scheduler
		) == wt::WtPageMeshingRuntimeStatus::CompletionNotOwned ? 1U : 0U;
	}
	check(
		stale_count == kDependencyCount &&
		runtime.record_count() == 0 &&
		runtime.pinned_page_count() == 0 &&
		scheduler.forget_chunk(fixture.coarse_key) ==
			wt::WtSchedulerStatus::Ok,
		"late cancelled completions mutated runtime state"
	);

	const wt::WtPageMeshingRuntimeMetrics metrics = runtime.get_metrics();
	check(
		metrics.sample_jobs == 2 && metrics.mesh_jobs == 1 &&
		metrics.dependency_requests == kDependencyCount * 2 &&
		metrics.accepted_storage_completions == kDependencyCount &&
		metrics.stale_storage_completions == kDependencyCount &&
		metrics.sample_successes == 1 && metrics.mesh_successes == 1 &&
		metrics.scheduler_backpressure == 1 &&
		metrics.cancellations == 1 &&
		metrics.invalidated_records == 1 &&
		metrics.maximum_pinned_pages == kDependencyCount,
		"page runtime metrics mismatch"
	);
	append_u64(evidence, mesh_hash);
	append_u64(evidence, metrics.dependency_requests);
	append_u64(evidence, metrics.accepted_storage_completions);
	append_u64(evidence, metrics.stale_storage_completions);
	append_u64(evidence, metrics.scheduler_backpressure);
	append_u64(evidence, metrics.maximum_pinned_pages);
	storage.close();
}

void test_missing_support(const RuntimeFixture &fixture) {
	wt::WtAsyncStorageService storage({ 32, 32, wt::kWtMaximumContainerSize });
	check(
		storage.open(fixture.incomplete_world_path, fixture.root) ==
			wt::WtAsyncStorageStatus::Ok,
		"incomplete runtime storage open failed"
	);
	wt::WtStoragePageCache cache({
		2,
		wt::kWtMaximumContainerSize,
		2,
		wt::kWtMaximumContainerSize,
	});
	wt::WtStreamScheduler scheduler(2, 2, 2, 1);
	wt::WtPageMeshingRuntimeService runtime(2);
	const wt::WtChunkJob sample = request_sample_job(
		scheduler,
		fixture.coarse_key,
		kWorldRevision,
		0
	);
	check(
		runtime.begin_sample_job(
			sample,
			fixture.transition_mask,
			storage,
			cache,
			scheduler
		) == wt::WtPageMeshingRuntimeStatus::StorageRequestFailure &&
		runtime.record_count() == 0 &&
		runtime.get_metrics().sample_failures == 1 &&
		scheduler.apply_completions(1) == 1 &&
		scheduler.find_record(fixture.coarse_key) != nullptr &&
		scheduler.find_record(fixture.coarse_key)->lifecycle ==
			wt::WtChunkLifecycle::Failed,
		"missing transition support did not fail the generation"
	);
	storage.close();

	wt::WtPageMeshingRuntimeService invalid(0);
	check(!invalid.valid(), "zero page runtime capacity was accepted");
	wt::WtChunkJob lod0_job = sample;
	lod0_job.key = { 0, 0, 0, 0 };
	check(
		runtime.begin_sample_job(
			lod0_job,
			wt::wt_face_bit(wt::WtChunkFace::PositiveX),
			storage,
			cache,
			scheduler
		) == wt::WtPageMeshingRuntimeStatus::InvalidConfiguration,
		"closed storage did not reject runtime work"
	);
}

} // namespace

int main() {
	RuntimeFixture fixture = make_fixture();
	check(
		fixture.pages.size() == kDependencyCount &&
		fixture.support_keys.size() == kDependencyCount - 1,
		"runtime fixture dependency count mismatch"
	);
	std::vector<std::uint8_t> evidence;
	test_runtime_lifecycle(fixture, evidence);
	test_missing_support(fixture);
	if (failure_count != 0) {
		std::fprintf(stderr, "M5_PAGE_MESHING_RUNTIME_FAIL failures=%d\n",
			failure_count);
		return 1;
	}
	std::printf("M5_PAGE_MESHING_RUNTIME_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"M5_PAGE_MESHING_RUNTIME_PASS dependencies=13 cache_entries=2 "
		"backpressure=1 cancellations=1 invalidations=1 missing_support=1\n"
	);
	return 0;
}
