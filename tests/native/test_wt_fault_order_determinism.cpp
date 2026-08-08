#include "editing/wt_edit_journal.h"
#include "editing/wt_edit_transaction.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_hash256.h"
#include "storage/wt_procedural_world_source.h"
#include "storage/wt_world_snapshot_store.h"
#include "streaming/wt_multi_viewer_desired_set.h"
#include "streaming/wt_stream_scheduler.h"
#include "telemetry/wt_determinism_trace.h"
#include "testing/wt_fault_injection.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
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

std::uint64_t mix(std::uint64_t hash, std::uint64_t value) noexcept {
	for (std::size_t byte = 0; byte < 8; ++byte) {
		hash ^= static_cast<std::uint8_t>(value >> (byte * 8U));
		hash *= 1099511628211ULL;
	}
	return hash;
}

std::uint64_t record_signature(const wt::WtChunkRecord &record) noexcept {
	std::uint64_t hash = 1469598103934665603ULL;
	hash = mix(hash, static_cast<std::uint32_t>(record.key.x));
	hash = mix(hash, static_cast<std::uint32_t>(record.key.y));
	hash = mix(hash, static_cast<std::uint32_t>(record.key.z));
	hash = mix(hash, record.key.lod);
	hash = mix(hash, record.generation.value);
	hash = mix(hash, record.source_revision);
	hash = mix(hash, record.world_revision);
	hash = mix(hash, static_cast<std::uint8_t>(record.lifecycle));
	return hash;
}

std::vector<wt::WtDeterminismTraceEvent> run_completion_order(
	std::uint32_t seed,
	std::uint64_t &stale_results,
	std::uint64_t &cancellations
) {
	wt::WtStreamScheduler scheduler(32, 64, 64, 2);
	std::vector<wt::WtChunkKey> keys;
	for (std::int32_t x = 0; x < 8; ++x) {
		keys.push_back({ x, 0, 0, 0 });
		check(scheduler.request_chunk_version(
			keys.back(), 7001, 11, 100 - x
		) == wt::WtSchedulerStatus::Ok, "scheduler request failed");
	}

	std::vector<wt::WtChunkJob> sample_jobs;
	wt::WtChunkJob job;
	while (scheduler.pop_job(job)) sample_jobs.push_back(job);
	check(sample_jobs.size() == keys.size(), "initial sample job count changed");

	check(scheduler.cancel_chunk(keys.front()) == wt::WtSchedulerStatus::Ok,
		"chunk cancellation failed");
	check(scheduler.request_chunk_version(
		keys.front(), 7001, 12, 120, true
	) == wt::WtSchedulerStatus::Ok, "replacement request failed");
	check(scheduler.pop_job(job), "replacement sample job missing");
	sample_jobs.push_back(job);

	std::vector<wt::WtChunkJobResult> sample_results;
	for (const wt::WtChunkJob &sample : sample_jobs) {
		sample_results.push_back({
			sample.key,
			sample.generation,
			wt::WtChunkJobStage::Sample,
			sample.key != keys.back(),
		});
	}
	sample_results.push_back(sample_results[1]);
	std::mt19937 random(seed);
	std::shuffle(sample_results.begin(), sample_results.end(), random);
	for (const wt::WtChunkJobResult &result : sample_results) {
		check(scheduler.submit_completion(result) == wt::WtSchedulerStatus::Ok,
			"sample completion queue rejected deterministic workload");
	}
	scheduler.apply_completions(sample_results.size());

	std::vector<wt::WtChunkJobResult> mesh_results;
	while (scheduler.pop_job(job)) {
		check(job.stage == wt::WtChunkJobStage::Mesh,
			"non-mesh job followed sample completion");
		mesh_results.push_back({
			job.key, job.generation, wt::WtChunkJobStage::Mesh, true
		});
	}
	check(mesh_results.size() == keys.size() - 1,
		"failed sample unexpectedly produced mesh work");
	mesh_results.push_back(mesh_results.front());
	std::shuffle(mesh_results.begin(), mesh_results.end(), random);
	for (const wt::WtChunkJobResult &result : mesh_results) {
		check(scheduler.submit_completion(result) == wt::WtSchedulerStatus::Ok,
			"mesh completion queue rejected deterministic workload");
	}
	scheduler.apply_completions(mesh_results.size());

	std::vector<wt::WtDeterminismTraceEvent> trace;
	for (const wt::WtChunkRecord &record : scheduler.get_records()) {
		const bool failed = record.lifecycle == wt::WtChunkLifecycle::Failed;
		check(failed || record.lifecycle == wt::WtChunkLifecycle::Ready,
			"scheduler did not settle to ready or fail-closed");
		trace.push_back({
			record.key,
			record.generation,
			failed ? wt::WtDeterminismEventKind::FailedClosed :
				wt::WtDeterminismEventKind::Published,
			record_signature(record),
			mix(1469598103934665603ULL, record.world_revision),
		});
	}
	const wt::WtSchedulerMetrics metrics = scheduler.get_metrics();
	stale_results = metrics.stale_results;
	cancellations = metrics.cancellations;
	return trace;
}

std::uint64_t run_rapid_viewer_motion(std::uint32_t seed) {
	wt::WtMultiViewerDesiredSet desired({ 2, 16, 32, 32 });
	check(desired.valid(), "desired set configuration rejected");
	std::vector<std::int32_t> motion { 0, 4, -3, 8, 1, 6 };
	std::mt19937 random(seed);
	std::shuffle(motion.begin(), motion.end(), random);
	wt::WtDesiredSetDelta delta;
	std::uint64_t revision = 1;
	for (std::int32_t x : motion) {
		const std::vector<wt::WtViewerChunkDemand> demands {
			{ { x, 0, 0, 0 }, 100, true, true },
			{ { x + 1, 0, 0, 1 }, 40, false, true },
		};
		check(desired.update_viewer(
			{ 41, static_cast<double>(x * 32), 0.0, 0.0, revision++ },
			demands,
			delta
		) == wt::WtMultiViewerDesiredSetStatus::Ok,
			"rapid viewer update failed");
	}
	const std::vector<wt::WtViewerChunkDemand> final_demands {
		{ { 2, 0, 2, 0 }, 200, true, true },
		{ { 1, 0, 1, 1 }, 80, false, true },
	};
	check(desired.update_viewer(
		{ 41, 64.0, 0.0, 64.0, 100 }, final_demands, delta
	) == wt::WtMultiViewerDesiredSetStatus::Ok,
		"final viewer settlement failed");
	check(desired.update_viewer(
		{ 41, 0.0, 0.0, 0.0, 99 }, final_demands, delta
	) == wt::WtMultiViewerDesiredSetStatus::StaleViewerRevision,
		"stale viewer revision did not fail closed");
	std::uint64_t hash = 1469598103934665603ULL;
	for (const wt::WtDesiredChunk &chunk : desired.get_desired_chunks()) {
		hash = mix(hash, static_cast<std::uint32_t>(chunk.key.x));
		hash = mix(hash, static_cast<std::uint32_t>(chunk.key.y));
		hash = mix(hash, static_cast<std::uint32_t>(chunk.key.z));
		hash = mix(hash, chunk.key.lod);
		hash = mix(hash, static_cast<std::uint32_t>(chunk.priority));
		hash = mix(hash, chunk.supporter_count);
		hash = mix(hash, chunk.collision_required ? 1 : 0);
	}
	return hash;
}

wt::WtProceduralWorldDescriptor descriptor() {
	wt::WtProceduralWorldDescriptor output;
	output.chunk_count_x = 2;
	output.chunk_count_y = 1;
	output.chunk_count_z = 2;
	output.chunk_y = 0;
	output.source_revision = 7001;
	output.world_revision = 0;
	output.seed = 43;
	output.mode = wt::WtProceduralWorldMode::Terrain;
	return output;
}

void run_async_fault_and_shutdown() {
	wt::WtAsyncStorageService storage({
		16, 16, wt::kWtMaximumContainerSize, 2
	});
	check(storage.open_procedural(descriptor()) == wt::WtAsyncStorageStatus::Ok,
		"procedural storage open failed");
	const wt::WtChunkKey key { 0, 0, 0, 0 };
	wt::wt_arm_fault_injection(
		wt::WtFaultInjectionSite::PageBufferAllocation, 1
	);
	check(storage.request_page(key, { 1 }, 100) == wt::WtAsyncStorageStatus::Ok,
		"faulted page request failed admission");
	check(storage.request_page(key, { 2 }, 50) ==
		wt::WtAsyncStorageStatus::AlreadyPending,
		"duplicate page request was not coalesced");
	wt::WtPageLoadCompletion completion;
	check(storage.wait_pop_completion(
		completion, std::chrono::seconds(10)
	) && completion.status == wt::WtPageLoadStatus::AllocationFailure &&
		!completion.page_bytes,
		"page allocation failure did not fail closed");
	const wt::WtFaultInjectionMetrics fault_metrics =
		wt::wt_fault_injection_metrics();
	check(fault_metrics.matching_attempts == 1 &&
		fault_metrics.injected_failures == 1 &&
		fault_metrics.remaining_matches == 0,
		"page allocation fault accounting changed");

	check(storage.request_page(key, { 3 }, 100) == wt::WtAsyncStorageStatus::Ok,
		"post-fault page request failed admission");
	check(storage.wait_pop_completion(
		completion, std::chrono::seconds(10)
	) && completion.status == wt::WtPageLoadStatus::Ok &&
		completion.page_bytes,
		"page service did not recover after injected failure");
	for (std::int32_t x = -1; x <= 2; ++x) {
		storage.request_page({ x, 0, 1, 0 },
			{ static_cast<std::uint64_t>(10 + x + 1) }, 10);
	}
	storage.close();
	const wt::WtAsyncStorageMetrics metrics = storage.get_metrics();
	check(!storage.is_open() && storage.queued_request_count() == 0 &&
		storage.queued_completion_count() == 0 &&
		storage.active_request_count() == 0,
		"async storage shutdown retained live work");
	check(metrics.accepted_requests ==
		metrics.completed_requests + metrics.cancelled_requests,
		"async shutdown request accounting is incomplete");
	wt::wt_clear_fault_injection();
}

bool write_marker(const std::filesystem::path &path) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	const char marker[] = "interrupted";
	output.write(marker, sizeof(marker));
	return output.good();
}

void run_snapshot_faults(const std::filesystem::path &root) {
	wt::WtAsyncStorageService storage({
		8, 8, wt::kWtMaximumContainerSize, 1
	});
	check(storage.open_procedural(descriptor()) == wt::WtAsyncStorageStatus::Ok,
		"snapshot source open failed");
	wt::WtEditJournal journal(8, 16, 1024 * 1024);
	journal.reset(7001, 0);
	wt::WtWorldSnapshotStoreResult result;
	const std::filesystem::path faulted = root / "snapshot-faulted";
	wt::wt_arm_fault_injection(
		wt::WtFaultInjectionSite::SnapshotWorkspaceAllocation, 1
	);
	check(wt::wt_write_migrated_world_snapshot(
		storage, journal, faulted, result
	) == wt::WtWorldSnapshotStoreStatus::AllocationFailure &&
		!std::filesystem::exists(faulted) &&
		!std::filesystem::exists(faulted.string() + ".tmp"),
		"snapshot allocation failure published partial state");
	const wt::WtFaultInjectionMetrics metrics = wt::wt_fault_injection_metrics();
	check(metrics.matching_attempts == 1 && metrics.injected_failures == 1,
		"snapshot allocation fault accounting changed");
	wt::wt_clear_fault_injection();

	const std::filesystem::path interrupted = root / "snapshot-interrupted";
	std::filesystem::create_directory(interrupted.string() + ".tmp");
	check(write_marker(
		interrupted.string() + ".tmp/interrupted.marker"
	), "interrupted snapshot marker write failed");
	check(wt::wt_write_migrated_world_snapshot(
		storage, journal, interrupted, result
	) == wt::WtWorldSnapshotStoreStatus::IoFailure &&
		!std::filesystem::exists(interrupted) &&
		std::filesystem::is_regular_file(
			interrupted.string() + ".tmp/interrupted.marker"
		), "interrupted snapshot publication did not fail closed");

	check(wt::wt_write_migrated_world_snapshot(
		storage, journal, {}, result
	) == wt::WtWorldSnapshotStoreStatus::InvalidInput,
		"malformed snapshot output path was accepted");
	const std::filesystem::path valid = root / "snapshot-valid";
	check(wt::wt_write_migrated_world_snapshot(
		storage, journal, valid, result
	) == wt::WtWorldSnapshotStoreStatus::Ok,
		"snapshot did not recover after fail-closed cases");
	wt::WtAsyncStorageService reopened({
		8, 8, wt::kWtMaximumContainerSize, 1
	});
	check(reopened.open_procedural_snapshot(valid) == wt::WtAsyncStorageStatus::Ok,
		"recovered snapshot did not reopen");
	reopened.close();

	const std::filesystem::path malformed = root / "snapshot-malformed";
	std::filesystem::create_directory(malformed);
	check(write_marker(malformed / "world.wtproc"),
		"malformed snapshot fixture write failed");
	std::error_code copy_error;
	check(std::filesystem::copy_file(
		valid / "world.wtworld",
		malformed / "world.wtworld",
		copy_error
	), "malformed snapshot manifest fixture copy failed");
	wt::WtAsyncStorageService malformed_storage({
		8, 8, wt::kWtMaximumContainerSize, 1
	});
	check(malformed_storage.open_procedural_snapshot(malformed) ==
		wt::WtAsyncStorageStatus::ManifestFailure,
		"malformed snapshot descriptor did not fail closed");
	storage.close();
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

} // namespace

int main() {
	std::uint64_t stale_results = 0;
	std::uint64_t cancellations = 0;
	const std::vector<wt::WtDeterminismTraceEvent> canonical =
		run_completion_order(0, stale_results, cancellations);
	check(canonical.size() == 8 && stale_results == 3 && cancellations == 1,
		"canonical scheduler fault metrics changed");
	std::uint64_t viewer_signature = run_rapid_viewer_motion(0);
	for (std::uint32_t seed = 1; seed < 64; ++seed) {
		std::uint64_t varied_stale = 0;
		std::uint64_t varied_cancellations = 0;
		const auto varied = run_completion_order(
			seed, varied_stale, varied_cancellations
		);
		check(wt::wt_compare_determinism_traces(canonical, varied).matches(),
			"completion order changed authoritative trace");
		check(varied_stale == stale_results &&
			varied_cancellations == cancellations,
			"completion order changed rejection accounting");
		check(run_rapid_viewer_motion(seed) == viewer_signature,
			"rapid viewer motion changed settled desired state");
	}

	auto divergent = canonical;
	divergent[3].resource_signature ^= 1U;
	const wt::WtDeterminismTraceComparison comparison =
		wt::wt_compare_determinism_traces(canonical, divergent);
	check(!comparison.matches() && comparison.event_index == 3 &&
		comparison.first_divergent_generation ==
			canonical[3].generation.value,
		"trace comparator did not identify first divergent generation");

	run_async_fault_and_shutdown();
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() /
		("world-transvoxel-tqp43-" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()
		));
	std::filesystem::create_directories(root);
	run_snapshot_faults(root);
	std::error_code cleanup_error;
	std::filesystem::remove_all(root, cleanup_error);
	check(!cleanup_error, "fault fixture cleanup failed");

	if (failure_count != 0) {
		std::fprintf(stderr,
			"FAULT_ORDER_DETERMINISM_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::string semantics = "orders=64;records=8;stale=3;cancellations=1;";
	semantics += "viewer=" + std::to_string(viewer_signature) + ";";
	semantics += "divergence_index=3;divergence_generation=" +
		std::to_string(comparison.first_divergent_generation) + ";";
	semantics += "page_allocation=fail_closed;snapshot_allocation=fail_closed;";
	semantics += "interruption=fail_closed;malformed=fail_closed;shutdown=drained";
	std::printf("FAULT_ORDER_DETERMINISM_HASH ");
	print_hash(wt::wt_sha256(
		reinterpret_cast<const std::uint8_t *>(semantics.data()),
		semantics.size()
	));
	std::printf(
		"FAULT_ORDER_DETERMINISM_PASS orders=64 records=8 stale=3 "
		"cancellations=1 allocation_faults=2 interruption=1 malformed=2 "
		"shutdown=drained first_divergence_generation=%llu\n",
		static_cast<unsigned long long>(comparison.first_divergent_generation)
	);
	return 0;
}
