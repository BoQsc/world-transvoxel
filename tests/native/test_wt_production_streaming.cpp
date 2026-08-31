#include "services/wt_read_only_world_runtime.h"
#include "diagnostics/wt_gpu_meshing_shadow.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_hash256.h"
#include "streaming/wt_balanced_lod_planner.h"
#include "streaming/wt_foreground_priority.h"
#include "wt_production_world_fixture.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
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

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
	for (std::size_t index = 0; index < 8; ++index) {
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
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
			("wt_production_streaming_" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()
			));
	}
	~FixtureRoot() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
	std::filesystem::path path;
};

struct PublicationCounts {
	std::size_t expects = 0;
	std::size_t removals = 0;
	std::size_t renders = 0;
	std::size_t collisions = 0;
	std::size_t render_vertices = 0;
	std::size_t render_indices = 0;
	std::size_t collision_only_expects = 0;
	bool collision_before_first_render = false;
	wt::WtChunkKey first_expect_key;
	wt::WtGenerationToken first_expect_generation;
};

bool collect_until(
	wt::WtReadOnlyWorldRuntime &runtime,
	PublicationCounts &counts,
	std::size_t expected_renders,
	std::size_t expected_collisions,
	std::vector<std::uint8_t> &evidence
) {
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(5);
	while (std::chrono::steady_clock::now() < deadline) {
		wt::WtReadOnlyPublication publication;
		bool consumed = false;
		while (runtime.pop_publication(publication)) {
			consumed = true;
			switch (publication.kind) {
				case wt::WtReadOnlyPublicationKind::ExpectChunk:
					++counts.expects;
					if (counts.first_expect_generation.value == 0) {
						counts.first_expect_key = publication.key;
						counts.first_expect_generation = publication.generation;
					}
					if (!publication.visual_required &&
						publication.collision_required) {
						++counts.collision_only_expects;
					}
					break;
				case wt::WtReadOnlyPublicationKind::RemoveChunk:
					++counts.removals;
					break;
				case wt::WtReadOnlyPublicationKind::RenderPayload:
					++counts.renders;
					if (publication.render) {
						counts.render_vertices += publication.render->vertices.size();
						counts.render_indices += publication.render->indices.size();
						if (evidence.empty()) {
							append_u64(evidence, publication.render->vertices.size());
							append_u64(evidence, publication.render->indices.size());
						}
					}
					break;
				case wt::WtReadOnlyPublicationKind::CollisionPayload:
					counts.collision_before_first_render =
						counts.collision_before_first_render || counts.renders == 0;
					++counts.collisions;
					break;
				case wt::WtReadOnlyPublicationKind::SetCollisionRequired:
				case wt::WtReadOnlyPublicationKind::SetVisualRequired:
				case wt::WtReadOnlyPublicationKind::EditCommitted:
				case wt::WtReadOnlyPublicationKind::EditRejected:
				case wt::WtReadOnlyPublicationKind::AuthoritativeSampleReady:
				case wt::WtReadOnlyPublicationKind::AuthoritativeSampleRejected:
				case wt::WtReadOnlyPublicationKind::AuthoritativeSampleBatchReady:
				case wt::WtReadOnlyPublicationKind::AuthoritativeSampleBatchRejected:
				case wt::WtReadOnlyPublicationKind::WorldSnapshotReady:
				case wt::WtReadOnlyPublicationKind::WorldSnapshotRejected:
				case wt::WtReadOnlyPublicationKind::ViewerPlanStarted:
				case wt::WtReadOnlyPublicationKind::ViewerPlanCompleted:
					break;
			}
		}
		if (counts.renders >= expected_renders &&
			counts.collisions >= expected_collisions) {
			return true;
		}
		if (!consumed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

wt::WtViewerSnapshot viewer(
	std::uint64_t id,
	std::uint64_t revision,
	double x,
	double y
) {
	return { id, x, y, 8.0, revision };
}

const wt::WtLodMapEntry *find_entry(
	const wt::WtBalancedLodPlan &plan,
	const wt::WtChunkKey &key
) {
	for (const wt::WtLodMapEntry &entry : plan.entries) {
		if (entry.key == key) return &entry;
	}
	return nullptr;
}

void test_g8_2000x2000_window_planning() {
	FixtureRoot fixture;
	std::filesystem::path world_path;
	check(wtt::wt_write_production_g8_2000x2000_fixture(
		fixture.path, 8101, 14, world_path
	), "g8 2000x2000 sparse fixture write failed");

	wt::WtAsyncStorageService storage({ 256, 256, wt::kWtMaximumContainerSize });
	check(storage.open(world_path, fixture.path) ==
		wt::WtAsyncStorageStatus::Ok,
		"g8 2000x2000 sparse fixture open failed");
	check(storage.page_count() == 93 &&
		storage.has_page({ 0, 0, 0, 0 }) &&
		storage.has_page({ 31, 0, 31, 0 }) &&
		storage.has_page({ 62, 0, 62, 0 }) &&
		storage.has_page({ 94, 0, 31, 0 }) &&
		storage.has_page({ 124, 0, 124, 0 }) &&
		!storage.has_page({ 125, 0, 124, 0 }),
		"g8 2000x2000 page catalog mismatch");

	struct Sample {
		double x;
		double z;
		std::size_t expected_entries;
		wt::WtChunkKey center_key;
	};
	const std::array<Sample, 5> samples = { {
		{ 8.0, 8.0, 9, { 0, 0, 0, 0 } },
		{ 496.0, 496.0, 25, { 31, 0, 31, 0 } },
		{ 1000.0, 1000.0, 25, { 62, 0, 62, 0 } },
		{ 1504.0, 496.0, 25, { 94, 0, 31, 0 } },
		{ 1991.0, 1991.0, 9, { 124, 0, 124, 0 } },
	} };
	wt::WtBalancedLodPlanner planner(256, storage.page_keys());
	check(planner.valid(), "g8 2000x2000 planner configuration rejected");
	std::size_t maximum_window = 0;
	std::uint64_t revision = 1;
	for (const Sample &sample : samples) {
		wt::WtBalancedLodPlan plan;
		check(planner.plan(
			{ { { 1, sample.x, 8.0, sample.z, revision++ }, 2, 0 } },
			{}, {}, plan
		) == wt::WtBalancedLodPlannerStatus::Ok,
			"g8 2000x2000 bounded window planning failed");
		check(plan.entries.size() == sample.expected_entries &&
			find_entry(plan, sample.center_key) != nullptr,
			"g8 2000x2000 bounded window shape mismatch");
		maximum_window = std::max(maximum_window, plan.entries.size());
	}
	check(maximum_window == 25,
		"g8 2000x2000 maximum bounded window mismatch");
	std::printf(
		"PRODUCTION_G8_2000X2000_WINDOW_PASS pages=93 samples=5 max_window=25\n"
	);
}

void test_visibility_coverage_priority_generation_contract() {
	FixtureRoot fixture;
	std::filesystem::path world_path;
	check(wtt::wt_write_production_streaming_fixture(
		fixture.path, 7002, 12, world_path
	), "priority fixture write failed");

	wt::WtAsyncStorageService storage({ 16, 16, wt::kWtMaximumContainerSize });
	check(storage.open(world_path, fixture.path) ==
		wt::WtAsyncStorageStatus::Ok,
		"priority fixture open failed");
	wt::WtRuntimeConfig config;
	config.active_chunk_capacity = 8;
	config.viewer_capacity = 1;
	config.demand_capacity_per_viewer = 125;
	config.storage_request_capacity = 16;
	config.storage_completion_capacity = 16;
	config.encoded_page_entry_capacity = 8;
	config.decoded_page_entry_capacity = 8;
	config.mesh_entry_capacity = 8;
	config.render_entry_capacity = 8;
	config.collision_entry_capacity = 8;
	wt::WtReadOnlyWorldRuntime runtime(config, storage);
	check(runtime.valid(), "priority runtime configuration rejected");
	check(runtime.begin_causal_trace(), "priority causal trace start failed");
	std::atomic<wt::WtReadOnlyRuntimeStatus> run_status {
		wt::WtReadOnlyRuntimeStatus::Ok
	};
	std::thread worker([&]() { run_status.store(runtime.run()); });

	check(runtime.update_viewer(viewer(1, 1, 8.0, 8.0), 0) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"priority viewer update rejected");
	PublicationCounts counts;
	std::vector<std::uint8_t> evidence;
	check(collect_until(runtime, counts, 1, 1, evidence),
		"priority fixture page did not publish");
	check(
		counts.first_expect_generation.value != 0 &&
		runtime.request_visibility_coverage_priority_batch({
			{
				counts.first_expect_key,
				counts.first_expect_generation,
			},
			{
				counts.first_expect_key,
				{ counts.first_expect_generation.value + 1U },
			},
		}) == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.request_visibility_coverage_priority_batch({}) ==
			wt::WtReadOnlyRuntimeStatus::InvalidEdit,
		"visibility coverage priority requests were rejected"
	);
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline) {
		const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
		if (metrics.visibility_coverage_priority_requests >= 2 &&
			metrics.visibility_coverage_priority_applied >= 1 &&
			metrics.visibility_coverage_priority_stale >= 1) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
	check(
		metrics.visibility_coverage_priority_requests == 2 &&
		metrics.visibility_coverage_priority_applied == 1 &&
		metrics.visibility_coverage_priority_stale == 1,
		"visibility coverage priority generation contract failed"
	);
	runtime.end_causal_trace();
	const wt::WtCausalTraceSnapshot trace = runtime.causal_trace_snapshot(0, 256);
	std::size_t applied_outcomes = 0;
	std::size_t stale_outcomes = 0;
	for (const wt::WtCausalTraceEvent &event : trace.events) {
		if (event.kind !=
				wt::WtCausalTraceEventKind::VisibilityCoveragePriorityOutcome) {
			continue;
		}
		if (event.status == static_cast<std::int64_t>(
				wt::WtVisibilityCoveragePriorityOutcome::Applied) ||
			event.status == static_cast<std::int64_t>(
				wt::WtVisibilityCoveragePriorityOutcome::
					SchedulerAppliedPageRecordNotFound)) {
			++applied_outcomes;
		} else if (event.status == static_cast<std::int64_t>(
				wt::WtVisibilityCoveragePriorityOutcome::
					SchedulerGenerationStale)) {
			++stale_outcomes;
		}
	}
	check(
		applied_outcomes == 1 && stale_outcomes == 1,
		"visibility coverage priority outcomes were not exhaustive"
	);
	runtime.request_stop();
	worker.join();
	storage.close();
	check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok,
		"priority runtime did not stop cleanly");
}

void test_foreground_priority_lease_contract() {
	const wt::WtChunkKey support_key{ 0, 0, 0, 0 };
	const wt::WtChunkKey old_focus_key{ 1, 0, 0, 0 };
	const wt::WtChunkKey new_focus_key{ 2, 0, 0, 0 };
	const std::vector<wt::WtViewerChunkDemand> initial_base{
		{ support_key, 100, true, true },
		{ old_focus_key, 200, false, true },
		{ new_focus_key, 300, false, true },
	};
	wt::WtForegroundPriorityLeaseSet leases;
	check(
		wt::kWtCommittedEditPriority > wt::kWtPlayerSupportPriority &&
		wt::kWtPlayerSupportPriority > wt::kWtInteractionFocusPriority,
		"foreground priority class order is invalid"
	);
	check(leases.update({
		1,
		1,
		wt::WtForegroundPriorityClass::PlayerSupport,
		{ support_key },
	}) == wt::WtForegroundPriorityStatus::Ok,
		"player support priority lease was rejected");
	check(leases.update({
		2,
		1,
		wt::WtForegroundPriorityClass::InteractionFocus,
		{ old_focus_key },
	}) == wt::WtForegroundPriorityStatus::Ok,
		"interaction focus priority lease was rejected");
	std::vector<wt::WtViewerChunkDemand> effective;
	wt::WtForegroundPriorityOverlayResult overlay = leases.apply(
		initial_base,
		effective
	);
	check(
		effective.size() == initial_base.size() &&
		effective[0].priority == wt::kWtPlayerSupportPriority &&
		effective[1].priority == wt::kWtInteractionFocusPriority &&
		effective[2].priority == initial_base[2].priority &&
		effective[0].collision_required ==
			initial_base[0].collision_required &&
		effective[1].visual_required == initial_base[1].visual_required &&
		overlay.requested_keys == 2 && overlay.matched_keys == 2 &&
		overlay.missing_keys == 0 && overlay.changed_priorities == 2,
		"foreground overlay changed topology, flags, or wrong priorities"
	);
	check(leases.update({
		2,
		2,
		wt::WtForegroundPriorityClass::InteractionFocus,
		{ new_focus_key },
	}) == wt::WtForegroundPriorityStatus::Ok,
		"interaction focus move was rejected");
	overlay = leases.apply(initial_base, effective);
	check(
		effective[1].priority == initial_base[1].priority &&
		effective[2].priority == wt::kWtInteractionFocusPriority,
		"interaction focus move did not restore the old base priority"
	);
	const std::vector<wt::WtViewerChunkDemand> updated_base{
		{ support_key, 400, true, true },
		{ old_focus_key, 500, false, true },
		{ new_focus_key, 600, false, true },
	};
	check(leases.update({
		2,
		3,
		wt::WtForegroundPriorityClass::InteractionFocus,
		{},
	}) == wt::WtForegroundPriorityStatus::Ok,
		"interaction focus release was rejected");
	leases.apply(updated_base, effective);
	check(
		effective[1].priority == updated_base[1].priority &&
		effective[2].priority == updated_base[2].priority,
		"focus release did not restore the latest viewer-plan priorities"
	);
	check(leases.update({
		2,
		2,
		wt::WtForegroundPriorityClass::InteractionFocus,
		{ old_focus_key },
	}) == wt::WtForegroundPriorityStatus::StaleRevision,
		"stale foreground priority revision was accepted");
	leases.apply(updated_base, effective);
	check(
		effective[1].priority == updated_base[1].priority &&
		effective[2].priority == updated_base[2].priority,
		"stale foreground request mutated effective priorities"
	);
	std::printf(
		"FOREGROUND_PRIORITY_LEASE_PASS sources=%zu active=%zu support=%zu focus=%zu\n",
		leases.source_count(),
		leases.active_source_count(),
		leases.active_key_count(
			wt::WtForegroundPriorityClass::PlayerSupport
		),
		leases.active_key_count(
			wt::WtForegroundPriorityClass::InteractionFocus
		)
	);
}

void test_foreground_priority_runtime_contract() {
	FixtureRoot fixture;
	std::filesystem::path world_path;
	check(wtt::wt_write_production_streaming_fixture(
		fixture.path, 7003, 12, world_path
	), "foreground runtime fixture write failed");
	wt::WtAsyncStorageService storage({
		16, 16, wt::kWtMaximumContainerSize
	});
	check(storage.open(world_path, fixture.path) ==
		wt::WtAsyncStorageStatus::Ok,
		"foreground runtime fixture open failed");
	wt::WtRuntimeConfig config;
	config.active_chunk_capacity = 8;
	config.viewer_capacity = 1;
	config.demand_capacity_per_viewer = 125;
	config.storage_request_capacity = 16;
	config.storage_completion_capacity = 16;
	config.encoded_page_entry_capacity = 8;
	config.decoded_page_entry_capacity = 8;
	config.mesh_entry_capacity = 8;
	config.render_entry_capacity = 8;
	config.collision_entry_capacity = 8;
	config.collision_activation_distance = 0.0;
	config.collision_deactivation_distance = 0.0;
	wt::WtReadOnlyWorldRuntime runtime(config, storage);
	check(runtime.valid(), "foreground runtime configuration rejected");
	check(runtime.begin_causal_trace(),
		"foreground runtime causal trace start failed");
	check(runtime.update_viewer(viewer(1, 1, 8.0, 8.0), 1) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"foreground runtime viewer update rejected");
	check(runtime.update_foreground_priority_lease({
		1,
		1,
		wt::WtForegroundPriorityClass::PlayerSupport,
		{ { 0, 0, 0, 0 } },
	}) == wt::WtReadOnlyRuntimeStatus::Ok,
		"foreground runtime support lease rejected");
	check(runtime.update_foreground_priority_lease({
		2,
		1,
		wt::WtForegroundPriorityClass::InteractionFocus,
		{ { 1, 0, 0, 0 } },
	}) == wt::WtReadOnlyRuntimeStatus::Ok,
		"foreground runtime focus lease rejected");
	std::atomic<wt::WtReadOnlyRuntimeStatus> run_status{
		wt::WtReadOnlyRuntimeStatus::Ok
	};
	std::thread worker([&]() { run_status.store(runtime.run()); });
	PublicationCounts counts;
	std::vector<std::uint8_t> evidence;
	check(collect_until(runtime, counts, 3, 1, evidence),
		"foreground runtime pages did not publish");
	check(runtime.update_foreground_priority_lease({
		2,
		2,
		wt::WtForegroundPriorityClass::InteractionFocus,
		{ { -1, 0, 0, 0 } },
	}) == wt::WtReadOnlyRuntimeStatus::Ok,
		"foreground runtime focus move rejected");
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline) {
		const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
		if (metrics.foreground_priority_updates >= 3 &&
			metrics.foreground_priority_active_sources == 2 &&
			metrics.foreground_priority_support_keys == 1 &&
			metrics.foreground_priority_focus_keys == 1) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
	std::printf(
		"FOREGROUND_RUNTIME_METRICS updates=%llu active=%llu support=%llu focus=%llu changed=%llu matched=%llu missing=%llu\n",
		static_cast<unsigned long long>(metrics.foreground_priority_updates),
		static_cast<unsigned long long>(
			metrics.foreground_priority_active_sources
		),
		static_cast<unsigned long long>(
			metrics.foreground_priority_support_keys
		),
		static_cast<unsigned long long>(
			metrics.foreground_priority_focus_keys
		),
		static_cast<unsigned long long>(
			metrics.foreground_priority_changed_priorities
		),
		static_cast<unsigned long long>(
			metrics.foreground_priority_matched_keys
		),
		static_cast<unsigned long long>(
			metrics.foreground_priority_missing_keys
		)
	);
	check(
		metrics.foreground_priority_updates == 3 &&
		metrics.foreground_priority_active_sources == 2 &&
		metrics.foreground_priority_support_keys == 1 &&
		metrics.foreground_priority_focus_keys == 1 &&
		metrics.foreground_priority_changed_priorities >= 2,
		"foreground runtime metrics did not expose active leases"
	);
	runtime.end_causal_trace();
	const wt::WtCausalTraceSnapshot trace = runtime.causal_trace_snapshot(
		0, 1024
	);
	bool support_priority_seen = false;
	bool new_focus_priority_seen = false;
	bool old_focus_demoted = false;
	for (const wt::WtCausalTraceEvent &event : trace.events) {
		if (!event.has_chunk) continue;
		if (event.key == wt::WtChunkKey{ 0, 0, 0, 0 }) {
			if ((event.kind == wt::WtCausalTraceEventKind::ChunkDemandAccepted &&
					event.auxiliary == static_cast<std::uint64_t>(
						wt::kWtPlayerSupportPriority
					)) ||
				(event.kind ==
						wt::WtCausalTraceEventKind::ForegroundPriorityChanged &&
					event.status == wt::kWtPlayerSupportPriority)) {
				support_priority_seen = true;
			}
		}
		if (event.kind !=
				wt::WtCausalTraceEventKind::ForegroundPriorityChanged) {
			continue;
		}
		if (event.key == wt::WtChunkKey{ -1, 0, 0, 0 } &&
			event.status == wt::kWtInteractionFocusPriority) {
			new_focus_priority_seen = true;
		}
		if (event.key == wt::WtChunkKey{ 1, 0, 0, 0 } &&
			event.status < wt::kWtInteractionFocusPriority) {
			old_focus_demoted = true;
		}
	}
	check(support_priority_seen && new_focus_priority_seen &&
		old_focus_demoted,
		"foreground runtime trace did not prove promotion and demotion");
	runtime.request_stop();
	worker.join();
	storage.close();
	check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok,
		"foreground priority runtime did not stop cleanly");
}

void test_collision_only_with_full_gpu_queue(std::size_t mesh_workers) {
	const int failures_before = failure_count;
	FixtureRoot fixture;
	std::filesystem::path world_path;
	check(wtt::wt_write_production_streaming_fixture(
		fixture.path, 7001, 12, world_path
	), "GPU collision fixture write failed");
	wt::WtAsyncStorageService storage({ 16, 16, wt::kWtMaximumContainerSize });
	check(storage.open(world_path, fixture.path) == wt::WtAsyncStorageStatus::Ok,
		"GPU collision fixture open failed");
	auto gpu = std::make_shared<wt::WtGpuMeshingShadowQueue>();
	check(gpu->begin(2, true, wt::WtGpuMeshingCaptureStage::PreMeshField),
		"GPU collision queue start failed");
	wt::WtChunkJob occupied;
	occupied.key = { -1, 0, 0, 0 };
	occupied.generation = { 1 };
	const std::uint64_t reservation = gpu->reserve_capture_slots(occupied);
	check(reservation != 0 && gpu->metrics().reserved_capture_slots == 2,
		"GPU collision test did not fill capture capacity");

	wt::WtRuntimeConfig config;
	config.active_chunk_capacity = 8;
	config.viewer_capacity = 2;
	config.demand_capacity_per_viewer = 125;
	config.meshing_worker_count = mesh_workers;
	wt::WtReadOnlyWorldRuntime runtime(config, storage, nullptr, gpu);
	check(runtime.valid(), "GPU collision runtime invalid");
	std::atomic<wt::WtReadOnlyRuntimeStatus> status { wt::WtReadOnlyRuntimeStatus::Ok };
	std::thread worker([&]() { status.store(runtime.run()); });
	check(runtime.update_collision_viewer(viewer(1, 1, 40.0, 8.0), 0) ==
		wt::WtReadOnlyRuntimeStatus::Ok, "GPU collision-only viewer rejected");
	bool collision_ready = false;
	bool hidden_render = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!collision_ready && std::chrono::steady_clock::now() < deadline) {
		wt::WtReadOnlyPublication publication;
		while (runtime.pop_publication(publication)) {
			hidden_render |= publication.kind == wt::WtReadOnlyPublicationKind::RenderPayload;
			if (publication.kind == wt::WtReadOnlyPublicationKind::CollisionPayload &&
				publication.key == wt::WtChunkKey{ 2, 0, 0, 0 } && publication.collision) {
				collision_ready = !publication.collision->faces.empty();
			}
		}
		if (!collision_ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const auto metrics = gpu->metrics();
	check(collision_ready, "collision-only work blocked by full GPU visual queue");
	check(!hidden_render && metrics.captured_requests == 0 &&
		metrics.reserved_capture_slots == 2 && metrics.capture_reservation_attempts == 1,
		"collision-only work consumed GPU visual admission or published hidden render");
	// Promoting the same chunk to visual must still obey GPU backpressure.
	check(runtime.update_viewer(viewer(2, 1, 40.0, 8.0), 0) ==
		wt::WtReadOnlyRuntimeStatus::Ok, "GPU visual promotion rejected");
	const auto promotion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (gpu->metrics().capture_reservation_attempts <= 1 &&
		std::chrono::steady_clock::now() < promotion_deadline) {
		wt::WtReadOnlyPublication publication;
		while (runtime.pop_publication(publication)) {
			hidden_render |= publication.kind == wt::WtReadOnlyPublicationKind::RenderPayload;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	check(!hidden_render && gpu->metrics().capture_reservation_rejections > 0 &&
		gpu->metrics().captured_requests == 0,
		"visual promotion bypassed GPU capacity");
	gpu->release_capture_slots(reservation);
	PublicationCounts promoted;
	std::vector<std::uint8_t> evidence;
	check(collect_until(runtime, promoted, 1, 1, evidence) &&
		promoted.render_vertices == 0 && promoted.render_indices == 0 &&
		gpu->metrics().pre_mesh_field_captures > 0,
		"visual promotion failed to resume with resident input after capacity release");
	runtime.request_stop();
	worker.join();
	check(status.load() == wt::WtReadOnlyRuntimeStatus::Ok,
		"GPU collision runtime did not stop cleanly");
	if (failure_count == failures_before) {
		std::printf("GPU_COLLISION_ADMISSION_PASS mesh_workers=%zu\n", mesh_workers);
	}
	gpu->end();
	storage.close();
}

void test_collision_promotion_before_mesh(std::size_t mesh_workers) {
	const int failures_before = failure_count;
	FixtureRoot fixture;
	std::filesystem::path world_path;
	check(wtt::wt_write_production_streaming_fixture(fixture.path, 7003, 12, world_path),
		"queued collision promotion fixture failed");
	wt::WtAsyncStorageService storage({16, 16, wt::kWtMaximumContainerSize});
	check(storage.open(world_path, fixture.path) == wt::WtAsyncStorageStatus::Ok,
		"queued collision promotion storage failed");
	auto gpu = std::make_shared<wt::WtGpuMeshingShadowQueue>();
	check(gpu->begin(2, true, wt::WtGpuMeshingCaptureStage::PreMeshField),
		"queued collision promotion GPU queue failed");
	wt::WtChunkJob occupied;
	occupied.key = {-1, 0, 0, 0};
	occupied.generation = {1};
	const auto reservation = gpu->reserve_capture_slots(occupied);
	check(reservation != 0, "queued collision promotion admission barrier failed");
	wt::WtRuntimeConfig config;
	config.active_chunk_capacity = 8;
	config.viewer_capacity = 2;
	config.demand_capacity_per_viewer = 125;
	config.visual_viewer_collision_enabled = false;
	config.meshing_worker_count = mesh_workers;
	wt::WtReadOnlyWorldRuntime runtime(config, storage, nullptr, gpu);
	check(runtime.valid() && runtime.begin_causal_trace(), "queued collision promotion runtime failed");
	std::atomic<wt::WtReadOnlyRuntimeStatus> status {wt::WtReadOnlyRuntimeStatus::Ok};
	std::thread worker([&]() { status.store(runtime.run()); });
	const wt::WtChunkKey target {2, 0, 0, 0};
	check(runtime.update_viewer(viewer(1, 1, 40.0, 8.0), 0) == wt::WtReadOnlyRuntimeStatus::Ok,
		"queued collision promotion visual viewer rejected");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	wt::WtGenerationToken initial_generation;
	std::vector<wt::WtReadOnlyPublication> publications;
	const auto collect = [&]() {
		wt::WtReadOnlyPublication publication;
		while (runtime.pop_publication(publication)) {
			if (publication.key == target) {
				if (publication.kind == wt::WtReadOnlyPublicationKind::ExpectChunk &&
					initial_generation.value == 0) initial_generation = publication.generation;
				publications.push_back(std::move(publication));
			}
		}
	};
	while (gpu->metrics().capture_reservation_rejections == 0 && std::chrono::steady_clock::now() < deadline) {
		collect();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	check(gpu->metrics().capture_reservation_rejections != 0, "visual mesh never reached admission barrier");
	check(runtime.update_collision_viewer(viewer(2, 1, 40.0, 8.0), 0) == wt::WtReadOnlyRuntimeStatus::Ok,
		"queued collision promotion collision viewer rejected");
	while (runtime.get_metrics().collision_viewer_updates == 0 && std::chrono::steady_clock::now() < deadline) {
		collect();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	check(runtime.get_metrics().collision_viewer_updates == 1, "collision promotion was not applied before meshing");
	gpu->release_capture_slots(reservation);
	bool consumed = false;
	while (!consumed && std::chrono::steady_clock::now() < deadline) {
		collect();
		for (const auto &event : runtime.causal_trace_snapshot(0, 4096).events) {
			consumed |= event.kind == wt::WtCausalTraceEventKind::MeshCompletionConsumed && event.key == target;
		}
		if (!consumed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	runtime.request_stop();
	worker.join();
	collect();
	runtime.end_causal_trace();
	std::size_t remeshes = 0, mesh_jobs = 0;
	for (const auto &event : runtime.causal_trace_snapshot(0, 4096).events) {
		if (event.key != target) continue;
		remeshes += event.kind == wt::WtCausalTraceEventKind::TransitionRemeshGenerationCreated;
		mesh_jobs += event.kind == wt::WtCausalTraceEventKind::MeshStarted;
	}
	bool original_collision = false, original_render = false;
	for (const auto &publication : publications) {
		if (publication.generation != initial_generation) continue;
		original_collision |= publication.kind == wt::WtReadOnlyPublicationKind::CollisionPayload &&
			publication.collision && !publication.collision->faces.empty();
		original_render |= publication.kind == wt::WtReadOnlyPublicationKind::RenderPayload && publication.render &&
			publication.render->publication_source == wt::WtRenderPublicationSource::GpuResidentPlaceholder;
	}
	std::printf("GPU_QUEUED_COLLISION_PROMOTION workers=%zu generation=%llu mesh_jobs=%zu remeshes=%zu render=%d collision=%d\n",
		mesh_workers, static_cast<unsigned long long>(initial_generation.value), mesh_jobs, remeshes,
		original_render, original_collision);
	check(consumed && original_collision && original_render && mesh_jobs == 1 && remeshes == 0,
		"collision promotion before meshing discarded completed generation or queued redundant work");
	check(status.load() == wt::WtReadOnlyRuntimeStatus::Ok, "queued collision promotion runtime did not stop cleanly");
	gpu->end();
	storage.close();
	if (failure_count == failures_before) std::printf("GPU_QUEUED_COLLISION_PROMOTION_PASS workers=%zu\n", mesh_workers);
}

} // namespace

int main(int argc, char **argv) {
	if (argc == 2 && std::string(argv[1]) == "--collision-promotion") {
		test_collision_promotion_before_mesh(0);
		test_collision_promotion_before_mesh(1);
		return failure_count == 0 ? 0 : 1;
	}
	test_collision_promotion_before_mesh(0);
	test_collision_promotion_before_mesh(1);
	test_collision_only_with_full_gpu_queue(0);
	test_collision_only_with_full_gpu_queue(1);
	test_g8_2000x2000_window_planning();
	test_visibility_coverage_priority_generation_contract();
	test_foreground_priority_lease_contract();
	test_foreground_priority_runtime_contract();

	FixtureRoot fixture;
	std::filesystem::path world_path;
	check(wtt::wt_write_production_streaming_fixture(
		fixture.path, 7001, 12, world_path
	), "streaming fixture write failed");

	wt::WtAsyncStorageService storage({ 16, 16, wt::kWtMaximumContainerSize });
	check(storage.open(world_path, fixture.path) ==
		wt::WtAsyncStorageStatus::Ok,
		"streaming fixture open failed");
	check(storage.page_count() == 4 &&
		storage.has_page({ -1, 0, 0, 0 }) &&
		storage.has_page({ 2, 0, 0, 0 }) &&
		!storage.has_page({ 3, 0, 0, 0 }),
		"streaming page catalog mismatch");

	wt::WtRuntimeConfig config;
	config.active_chunk_capacity = 8;
	config.viewer_capacity = 2;
	config.demand_capacity_per_viewer = 125;
	config.meshing_worker_count = 2;
	config.storage_request_capacity = 16;
	config.storage_completion_capacity = 16;
	config.encoded_page_entry_capacity = 8;
	config.decoded_page_entry_capacity = 8;
	config.mesh_entry_capacity = 8;
	config.render_entry_capacity = 8;
	config.collision_entry_capacity = 8;
	wt::WtReadOnlyWorldRuntime runtime(config, storage);
	check(runtime.valid(), "read-only runtime configuration rejected");
	check(runtime.begin_causal_trace(), "parallel causal trace start failed");
	std::atomic<wt::WtReadOnlyRuntimeStatus> run_status {
		wt::WtReadOnlyRuntimeStatus::Ok
	};
	std::thread worker([&]() { run_status.store(runtime.run()); });

	check(runtime.update_viewer(viewer(1, 1, 8.0, 8.0), 0) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"initial viewer update rejected");
	check(runtime.update_viewer(viewer(9, 1, 8.0, 8.0), 3) ==
		wt::WtReadOnlyRuntimeStatus::InvalidViewer,
		"oversize viewer radius accepted");
	PublicationCounts counts;
	std::vector<std::uint8_t> evidence;
	check(collect_until(runtime, counts, 1, 1, evidence),
		"initial page did not publish render and collision");
	check(counts.expects == 1 && counts.collision_before_first_render &&
		counts.render_vertices != 0 &&
		counts.render_indices != 0,
		"initial page publication mismatch");
	const wt::WtReadOnlyRuntimeMetrics initial_metrics = runtime.get_metrics();
	check(
		initial_metrics.page_cache_encoded_entries <=
			config.encoded_page_entry_capacity &&
		initial_metrics.page_cache_encoded_resident_bytes <=
			config.encoded_page_byte_capacity &&
		initial_metrics.page_cache_decoded_entries <=
			config.decoded_page_entry_capacity &&
		initial_metrics.page_cache_decoded_resident_bytes <=
			config.decoded_page_byte_capacity &&
		initial_metrics.resource_cache_mesh_entries > 0 &&
		initial_metrics.resource_cache_mesh_entries <=
			config.mesh_entry_capacity &&
		initial_metrics.resource_cache_mesh_resident_bytes > 0 &&
		initial_metrics.resource_cache_mesh_resident_bytes <=
			config.mesh_byte_capacity &&
		initial_metrics.resource_cache_render_entries > 0 &&
		initial_metrics.resource_cache_render_entries <=
			config.render_entry_capacity &&
		initial_metrics.resource_cache_render_resident_bytes > 0 &&
		initial_metrics.resource_cache_render_resident_bytes <=
			config.render_byte_capacity &&
		initial_metrics.resource_cache_collision_entries > 0 &&
		initial_metrics.resource_cache_collision_entries <=
			config.collision_entry_capacity &&
		initial_metrics.resource_cache_collision_resident_bytes > 0 &&
		initial_metrics.resource_cache_collision_resident_bytes <=
			config.collision_byte_capacity,
		"runtime cache residency exceeded configured limits"
	);
	const std::size_t renders_before_collision_invoker = counts.renders;
	const std::size_t collisions_before_collision_invoker = counts.collisions;
	check(runtime.update_collision_viewer(
			viewer(1, 2, 40.0, 8.0), 0
		) == wt::WtReadOnlyRuntimeStatus::Ok,
		"collision-only viewer update rejected");
	check(collect_until(
			runtime,
			counts,
			renders_before_collision_invoker,
			collisions_before_collision_invoker + 1,
			evidence
		),
		"collision-only viewer did not publish collision");
	check(counts.renders == renders_before_collision_invoker &&
		counts.collision_only_expects >= 1,
		"collision-only viewer published hidden render work");
	const wt::WtCausalTraceSnapshot collision_invoker_trace =
		runtime.causal_trace_snapshot(0, 1024);
	bool collision_invoker_priority_seen = false;
	for (const wt::WtCausalTraceEvent &event :
			collision_invoker_trace.events) {
		if (event.kind == wt::WtCausalTraceEventKind::ChunkDemandAccepted &&
			event.has_chunk && event.key == wt::WtChunkKey{ 2, 0, 0, 0 } &&
			event.auxiliary == static_cast<std::uint64_t>(
				wt::kWtPlayerSupportPriority
			)) {
			collision_invoker_priority_seen = true;
			break;
		}
	}
	check(collision_invoker_priority_seen,
		"collision-only viewer entered the committed-edit priority band");
	check(runtime.remove_collision_viewer(1, 3) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"collision-only viewer removal rejected");

	check(runtime.update_viewer(viewer(1, 2, 24.0, 8.0), 0) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"moving viewer update rejected");
	check(collect_until(runtime, counts, 2, 2, evidence),
		"moving viewer page did not publish");
	check(counts.removals >= 1 && counts.expects >= 2,
		"moving viewer did not evict and request");

	check(runtime.update_viewer(viewer(2, 1, 8.0, -24.0), 2) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"underground second viewer update rejected");
	check(collect_until(runtime, counts, 4, 4, evidence),
		"multi-viewer underground pages did not publish");

	check(runtime.update_viewer(viewer(2, 2, 8.0, 40.0), 2) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"vertical viewer update rejected");
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	check(runtime.remove_viewer(1, 3) == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.remove_viewer(2, 3) == wt::WtReadOnlyRuntimeStatus::Ok,
		"viewer removal rejected");
	const auto removal_deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(3);
	while (std::chrono::steady_clock::now() < removal_deadline &&
		runtime.get_metrics().viewer_removals != 2) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	wt::WtReadOnlyPublication publication;
	while (runtime.pop_publication(publication)) {
		if (publication.kind == wt::WtReadOnlyPublicationKind::RemoveChunk) {
			++counts.removals;
		}
	}

	runtime.request_stop();
	worker.join();
	runtime.end_causal_trace();
	storage.close();
	const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
	const wt::WtCausalTraceSnapshot trace = runtime.causal_trace_snapshot(
		0,
		config.trace_event_capacity
	);
	std::size_t worker_mesh_starts = 0;
	std::size_t worker_mesh_finishes = 0;
	std::size_t incorrectly_attributed_mesh_events = 0;
	for (const wt::WtCausalTraceEvent &event : trace.events) {
		if (event.kind != wt::WtCausalTraceEventKind::MeshStarted &&
			event.kind != wt::WtCausalTraceEventKind::MeshFinished) {
			continue;
		}
		if (event.thread_role != wt::WtCausalTraceThreadRole::Meshing) {
			++incorrectly_attributed_mesh_events;
		} else if (event.kind == wt::WtCausalTraceEventKind::MeshStarted) {
			++worker_mesh_starts;
		} else {
			++worker_mesh_finishes;
		}
	}
	check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok,
		"read-only runtime did not stop cleanly");
	check(metrics.viewer_updates >= 4 && metrics.viewer_removals == 2 &&
		metrics.collision_viewer_updates == 1 &&
		metrics.collision_viewer_removals == 1 &&
		metrics.sample_jobs >= 4 && metrics.mesh_jobs >= 4 &&
		metrics.storage_completions >= 4 && metrics.mesh_completions >= 4 &&
		metrics.mesh_worker_count == 2 &&
		metrics.mesh_worker_accepted_jobs == metrics.mesh_jobs &&
		metrics.mesh_worker_started_jobs == metrics.mesh_worker_completed_jobs &&
		metrics.mesh_worker_maximum_active_jobs == 2 &&
		metrics.mesh_worker_queue_rejections == 0 &&
		metrics.mesh_job_time_ns_total != 0 &&
		worker_mesh_starts == metrics.mesh_worker_started_jobs &&
		worker_mesh_finishes == metrics.mesh_worker_completed_jobs &&
		worker_mesh_starts == worker_mesh_finishes &&
		incorrectly_attributed_mesh_events == 0,
		"read-only runtime metrics mismatch");
	append_u64(evidence, metrics.viewer_updates);
	append_u64(evidence, metrics.viewer_removals);
	append_u64(evidence, storage.page_count());

	if (failure_count != 0) {
		std::fprintf(stderr, "PRODUCTION_STREAMING_FAIL failures=%d\n",
			failure_count);
		return 1;
	}
	std::printf("PRODUCTION_STREAMING_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"PRODUCTION_STREAMING_PASS pages=4 viewers=2 movement=4 backend=MIT "
		"meshing_workers=2\n"
	);
	return 0;
}
