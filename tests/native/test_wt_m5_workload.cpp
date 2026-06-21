#include "wt_m5_workload_fixture.h"

#include "storage/wt_hash256.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <string_view>
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

enum class Axis {
	X,
	Y,
	Z,
};

std::vector<wt::WtViewerChunkDemand> line_demands(
	std::int32_t x,
	std::int32_t y,
	std::int32_t z,
	Axis axis
) {
	std::vector<wt::WtViewerChunkDemand> demands;
	for (std::int32_t offset = -2; offset <= 2; ++offset) {
		wt::WtChunkKey key = { x, y, z, 0 };
		if (axis == Axis::X) key.x += offset;
		else if (axis == Axis::Y) key.y += offset;
		else key.z += offset;
		demands.push_back({
			key,
			100 - std::abs(offset) * 10,
			offset == 0,
		});
	}
	return demands;
}

wt::WtViewerSnapshot viewer(
	std::uint64_t id,
	std::uint64_t revision,
	std::int32_t x,
	std::int32_t y,
	std::int32_t z
) {
	return {
		id,
		static_cast<double>(x) * 16.0 + 8.0,
		static_cast<double>(y) * 16.0 + 8.0,
		static_cast<double>(z) * 16.0 + 8.0,
		revision,
	};
}

wt::WtGridPoint chunk_center(
	std::int32_t x,
	std::int32_t y,
	std::int32_t z
) {
	return {
		static_cast<std::int64_t>(x) * 16 + 8,
		static_cast<std::int64_t>(y) * 16 + 8,
		static_cast<std::int64_t>(z) * 16 + 8,
	};
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

void test_runtime_delta_contract(std::vector<std::uint8_t> &evidence) {
	const wt::WtChunkKey key = { 0, 0, 0, 0 };
	wt::WtStreamScheduler scheduler(2, 2, 2, 0);
	wt::WtChunkApplicationService application(2, 2, 2);
	wt::WtStoragePageCache page_cache({
		2, wt::kWtMaximumContainerSize,
		2, wt::kWtMaximumContainerSize,
	});
	wt::WtChunkResourceCache resource_cache({
		2, wt::kWtMaximumResourceCacheBytes,
		2, wt::kWtMaximumResourceCacheBytes,
		2, wt::kWtMaximumResourceCacheBytes,
	});
	wt::WtDesiredSetRuntimeService runtime(4);
	wt::WtDesiredSetDelta delta;
	delta.added = { { key, 10, 1, false } };
	check(runtime.apply_delta(
		delta, 7, 3, scheduler, page_cache, resource_cache, application
	) == wt::WtDesiredSetRuntimeStatus::Ok,
		"runtime delta addition failed");
	const wt::WtChunkRecord *record = scheduler.find_record(key);
	check(record != nullptr && record->priority == 10 &&
		record->source_revision == 7 && record->world_revision == 3 &&
		application.find_record(key) != nullptr,
		"runtime delta addition state mismatch");

	delta.clear();
	delta.updated = { { key, 25, 2, true } };
	check(runtime.apply_delta(
		delta, 7, 3, scheduler, page_cache, resource_cache, application
	) == wt::WtDesiredSetRuntimeStatus::Ok,
		"runtime delta update failed");
	record = scheduler.find_record(key);
	const wt::WtChunkApplicationRecord *application_record =
		application.find_record(key);
	wt::WtChunkJob job;
	check(record != nullptr && record->priority == 25 &&
		application_record != nullptr && application_record->collision_required &&
		scheduler.pop_job(job) && job.priority == 25,
		"runtime delta update did not reprioritize queued work");

	check(scheduler.request_chunk_version({ 1, 0, 0, 0 }, 7, 3, 5) ==
		wt::WtSchedulerStatus::Ok,
		"runtime removal discard fixture request failed");
	const wt::WtChunkRecord *discarded =
		scheduler.find_record({ 1, 0, 0, 0 });
	check(discarded != nullptr &&
		application.expect_chunk(
			discarded->key, discarded->generation, false
		) == wt::WtApplicationStatus::Ok &&
		scheduler.submit_completion({
			discarded->key,
			discarded->generation,
			wt::WtChunkJobStage::Sample,
			true,
		}) == wt::WtSchedulerStatus::Ok,
		"runtime removal discard fixture application failed");
	delta.clear();
	delta.removed = { { 1, 0, 0, 0 } };
	check(runtime.apply_delta(
		delta, 7, 3, scheduler, page_cache, resource_cache, application
	) == wt::WtDesiredSetRuntimeStatus::Ok &&
		scheduler.find_record({ 1, 0, 0, 0 }) == nullptr &&
		application.find_record({ 1, 0, 0, 0 }) == nullptr &&
		scheduler.queued_job_count() == 0 &&
		scheduler.queued_completion_count() == 0 &&
		scheduler.get_metrics().discarded_jobs == 1 &&
		scheduler.get_metrics().discarded_completions == 1,
		"runtime removal retained records or queued work");

	wt::WtDesiredSetRuntimeService invalid(0);
	check(!invalid.valid() && invalid.apply_delta(
		{}, 7, 3, scheduler, page_cache, resource_cache, application
	) == wt::WtDesiredSetRuntimeStatus::InvalidConfiguration,
		"invalid runtime service accepted a delta");
	wt::WtDesiredSetRuntimeService small(1);
	delta.clear();
	delta.added = {
		{ { 2, 0, 0, 0 }, 1, 1, false },
		{ { 3, 0, 0, 0 }, 1, 1, false },
	};
	check(small.apply_delta(
		delta, 7, 3, scheduler, page_cache, resource_cache, application
	) == wt::WtDesiredSetRuntimeStatus::ChangeCapacityExceeded,
		"runtime change capacity overflow was accepted");
	delta.added = {
		{ { 3, 0, 0, 0 }, 1, 1, false },
		{ { 2, 0, 0, 0 }, 1, 1, false },
	};
	check(runtime.apply_delta(
		delta, 7, 3, scheduler, page_cache, resource_cache, application
	) == wt::WtDesiredSetRuntimeStatus::InvalidDelta,
		"noncanonical runtime delta was accepted");
	delta.clear();
	delta.updated = { { { 9, 0, 0, 0 }, 1, 1, false } };
	check(runtime.apply_delta(
		delta, 7, 3, scheduler, page_cache, resource_cache, application
	) == wt::WtDesiredSetRuntimeStatus::RuntimeStateMismatch,
		"missing runtime update state was accepted");
	delta.clear();
	delta.added = {
		{ { 2, 0, 0, 0 }, 1, 1, false },
		{ { 3, 0, 0, 0 }, 1, 1, false },
	};
	wt::WtStreamScheduler record_limited_scheduler(1, 2, 2, 0);
	wt::WtChunkApplicationService record_limited_application(1, 2, 2);
	check(runtime.apply_delta(
		delta, 7, 3,
		record_limited_scheduler,
		page_cache,
		resource_cache,
		record_limited_application
	) == wt::WtDesiredSetRuntimeStatus::RecordCapacityExceeded &&
		record_limited_scheduler.get_records().empty() &&
		record_limited_application.get_records().empty(),
		"runtime record capacity rejection mutated state");
	wt::WtStreamScheduler queue_limited_scheduler(2, 1, 2, 0);
	wt::WtChunkApplicationService queue_limited_application(2, 2, 2);
	check(runtime.apply_delta(
		delta, 7, 3,
		queue_limited_scheduler,
		page_cache,
		resource_cache,
		queue_limited_application
	) == wt::WtDesiredSetRuntimeStatus::JobQueueCapacityExceeded &&
		queue_limited_scheduler.get_records().empty() &&
		queue_limited_application.get_records().empty(),
		"runtime job capacity rejection mutated state");

	const wt::WtDesiredSetRuntimeMetrics metrics = runtime.get_metrics();
	check(metrics.applied_deltas == 3 && metrics.added_chunks == 1 &&
		metrics.removed_chunks == 1 && metrics.updated_chunks == 1 &&
		metrics.invalid_deltas == 1 && metrics.state_rejections == 1 &&
		metrics.capacity_rejections == 2,
		"runtime delta metrics mismatch");
	append_u64(evidence, metrics.applied_deltas);
	append_u64(evidence, metrics.added_chunks);
	append_u64(evidence, metrics.removed_chunks);
	append_u64(evidence, metrics.updated_chunks);
	append_u64(evidence, scheduler.get_metrics().discarded_jobs);
	append_u64(evidence, scheduler.get_metrics().discarded_completions);
}

void test_representative_workload(
	std::vector<std::uint8_t> &evidence,
	bool print_metrics
) {
	wtt::WtM5WorkloadFixture fixture;
	check(fixture.valid(), "representative workload fixture is invalid");
	std::uint64_t revision1 = 1;
	std::uint64_t revision2 = 1;
	std::int32_t x1 = 0;
	std::int32_t y1 = 0;
	std::int32_t z1 = 0;
	check(fixture.update_viewer(
		viewer(1, revision1, x1, y1, z1),
		line_demands(x1, y1, z1, Axis::X)
	), "initial representative viewer update failed");
	check(fixture.drain(64), "initial representative workload did not drain");

	for (int step = 1; step <= 32; ++step) {
		x1 += 3;
		++revision1;
		check(fixture.update_viewer(
			viewer(1, revision1, x1, y1, z1),
			line_demands(x1, y1, z1, Axis::X)
		), "fast-vehicle viewer update failed");
		if (step % 8 == 0) {
			check(fixture.apply_edit(chunk_center(x1, y1, z1)),
				"moving edit event failed");
		}
		check(fixture.run_frame(6, 2, 1),
			"fast-vehicle workload frame failed");
	}
	check(fixture.drain(128), "fast-vehicle workload did not drain");

	x1 = 512;
	++revision1;
	check(fixture.update_viewer(
		viewer(1, revision1, x1, y1, z1),
		line_demands(x1, y1, z1, Axis::X)
	), "teleport viewer update failed");
	check(fixture.scheduler().get_records().size() == 5,
		"teleport retained old scheduler records");
	check(fixture.drain(64), "teleport workload did not drain");

	y1 = -64;
	++revision1;
	check(fixture.update_viewer(
		viewer(1, revision1, x1, y1, z1),
		line_demands(x1, y1, z1, Axis::Y)
	), "underground entry update failed");
	for (int step = 0; step < 16; ++step) {
		y1 -= 2;
		++revision1;
		check(fixture.update_viewer(
			viewer(1, revision1, x1, y1, z1),
			line_demands(x1, y1, z1, Axis::Y)
		), "underground traversal update failed");
		check(fixture.run_frame(8, 3, 1),
			"underground traversal frame failed");
	}
	check(fixture.drain(96), "underground workload did not drain");

	y1 = 64;
	++revision1;
	check(fixture.update_viewer(
		viewer(1, revision1, x1, y1, z1),
		line_demands(x1, y1, z1, Axis::Y)
	), "vertical-world entry update failed");
	for (int step = 0; step < 16; ++step) {
		y1 += 4;
		++revision1;
		check(fixture.update_viewer(
			viewer(1, revision1, x1, y1, z1),
			line_demands(x1, y1, z1, Axis::Y)
		), "vertical-world traversal update failed");
		check(fixture.run_frame(8, 3, 1),
			"vertical-world traversal frame failed");
	}
	check(fixture.drain(96), "vertical-world workload did not drain");

	std::int32_t x2 = x1 + 32;
	std::int32_t y2 = y1;
	std::int32_t z2 = 32;
	check(fixture.update_viewer(
		viewer(2, revision2, x2, y2, z2),
		line_demands(x2, y2, z2, Axis::Z)
	), "second representative viewer update failed");
	for (int step = 0; step < 24; ++step) {
		++x1;
		--z2;
		++revision1;
		++revision2;
		check(fixture.update_viewer(
			viewer(1, revision1, x1, y1, z1),
			line_demands(x1, y1, z1, Axis::X)
		), "multi-viewer primary update failed");
		check(fixture.update_viewer(
			viewer(2, revision2, x2, y2, z2),
			line_demands(x2, y2, z2, Axis::Z)
		), "multi-viewer secondary update failed");
		if (step % 6 == 0) {
			check(fixture.apply_edit(chunk_center(x1, y1, z1)),
				"multi-viewer moving edit failed");
		}
		check(fixture.run_frame(8, 4, 2),
			"multi-viewer workload frame failed");
	}
	check(fixture.drain(160), "multi-viewer workload did not drain");
	check(fixture.remove_viewer(2, revision2 + 1),
		"second representative viewer removal failed");
	check(fixture.drain(64), "viewer-removal workload did not drain");

	const wtt::WtM5WorkloadMetrics metrics = fixture.get_metrics();
	const wt::WtSchedulerMetrics scheduler_metrics =
		fixture.scheduler().get_metrics();
	const wt::WtApplicationMetrics application_metrics =
		fixture.application().get_metrics();
	const wt::WtDesiredSetRuntimeMetrics runtime_metrics =
		fixture.runtime_service().get_metrics();
	const wt::WtEditRuntimeReplacementMetrics edit_metrics =
		fixture.edit_service().get_metrics();
	check(fixture.desired_set().get_desired_chunks().size() == 5 &&
		fixture.scheduler().get_records().size() == 5 &&
		fixture.application().get_records().size() == 5,
		"final representative runtime ownership mismatch");
	for (const wt::WtChunkApplicationRecord &record :
		fixture.application().get_records()) {
		check(record.fully_ready(),
			"final representative chunk is not fully ready");
	}
	check(metrics.viewer_events == 118 && metrics.edit_events == 8 &&
		fixture.world_revision() == 15,
		"representative event counts mismatch");
	check(metrics.maximum_desired_chunks <= 10 &&
		metrics.maximum_scheduler_records <= 10 &&
		metrics.maximum_job_queue < 128 &&
		metrics.maximum_render_queue < 128 &&
		metrics.maximum_collision_queue < 128 &&
		metrics.maximum_resource_entries <= 30,
		"representative workload exceeded construction bounds");
	check(metrics.maximum_readiness_latency_frames <= 32,
		"representative readiness latency exceeded the contract bound");
	check(scheduler_metrics.queue_rejections == 0 &&
		application_metrics.queue_rejections == 0 &&
		runtime_metrics.capacity_rejections == 0 &&
		runtime_metrics.state_rejections == 0 &&
		edit_metrics.capacity_rejections == 0 &&
		edit_metrics.state_rejections == 0,
		"representative workload hit a rejection path");
	check(scheduler_metrics.cancellations > 0 &&
		scheduler_metrics.stale_results > 0 &&
		application_metrics.applied_render > 0 &&
		application_metrics.applied_collision > 0 &&
		runtime_metrics.added_chunks > 0 &&
		runtime_metrics.removed_chunks > 0 &&
		runtime_metrics.updated_chunks > 0 &&
		edit_metrics.replaced_chunks > 0,
		"representative workload missed required runtime behavior");

	append_u64(evidence, metrics.frames);
	append_u64(evidence, metrics.viewer_events);
	append_u64(evidence, metrics.edit_events);
	append_u64(evidence, metrics.worker_jobs);
	append_u64(evidence, metrics.maximum_desired_chunks);
	append_u64(evidence, metrics.maximum_scheduler_records);
	append_u64(evidence, metrics.maximum_job_queue);
	append_u64(evidence, metrics.maximum_render_queue);
	append_u64(evidence, metrics.maximum_collision_queue);
	append_u64(evidence, metrics.maximum_resource_entries);
	append_u64(evidence, metrics.maximum_readiness_latency_frames);
	append_u64(evidence, scheduler_metrics.stale_results);
	append_u64(evidence, scheduler_metrics.cancellations);
	append_u64(evidence, scheduler_metrics.discarded_jobs);
	append_u64(evidence, scheduler_metrics.discarded_completions);
	append_u64(evidence, application_metrics.applied_render);
	append_u64(evidence, application_metrics.applied_collision);
	append_u64(evidence, runtime_metrics.added_chunks);
	append_u64(evidence, runtime_metrics.removed_chunks);
	append_u64(evidence, runtime_metrics.updated_chunks);
	append_u64(evidence, edit_metrics.replaced_chunks);
	if (print_metrics) {
		std::printf(
			"M5_WORKLOAD_METRICS frames=%llu worker_jobs=%llu desired_max=%zu "
			"records_max=%zu jobs_max=%zu render_max=%zu collision_max=%zu "
			"resources_max=%zu readiness_max=%llu stale=%llu "
			"discarded_jobs=%llu discarded_completions=%llu\n",
			static_cast<unsigned long long>(metrics.frames),
			static_cast<unsigned long long>(metrics.worker_jobs),
			metrics.maximum_desired_chunks,
			metrics.maximum_scheduler_records,
			metrics.maximum_job_queue,
			metrics.maximum_render_queue,
			metrics.maximum_collision_queue,
			metrics.maximum_resource_entries,
			static_cast<unsigned long long>(
				metrics.maximum_readiness_latency_frames
			),
			static_cast<unsigned long long>(scheduler_metrics.stale_results),
			static_cast<unsigned long long>(scheduler_metrics.discarded_jobs),
			static_cast<unsigned long long>(
				scheduler_metrics.discarded_completions
			)
		);
	}
}

bool parse_count(const char *text, std::size_t &output) {
	char *end = nullptr;
	const unsigned long long value = std::strtoull(text, &end, 10);
	if (end == text || *end != '\0' || value == 0 ||
		value > std::numeric_limits<std::size_t>::max()) {
		return false;
	}
	output = static_cast<std::size_t>(value);
	return true;
}

int run_benchmark(std::size_t runs, std::size_t warmup_runs) {
	using Clock = std::chrono::steady_clock;
	for (std::size_t iteration = 0;
		iteration < warmup_runs + runs;
		++iteration) {
		std::vector<std::uint8_t> evidence;
		const auto start = Clock::now();
		test_representative_workload(evidence, false);
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
			Clock::now() - start
		).count();
		if (failure_count != 0) {
			std::fprintf(stderr,
				"M5_WORKLOAD_BENCHMARK_FAIL failures=%d\n",
				failure_count);
			return 1;
		}
		if (iteration >= warmup_runs) {
			std::printf(
				"M5_WORKLOAD_BENCHMARK_SAMPLE run=%zu duration_ns=%lld\n",
				iteration - warmup_runs,
				static_cast<long long>(elapsed)
			);
		}
	}
	std::printf(
		"M5_WORKLOAD_BENCHMARK_PASS runs=%zu warmup=%zu frames_per_run=106\n",
		runs,
		warmup_runs
	);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 1) {
		std::size_t runs = 0;
		std::size_t warmup_runs = 0;
		if (argc != 5 ||
			std::string_view(argv[1]) != "--benchmark-runs" ||
			!parse_count(argv[2], runs) ||
			std::string_view(argv[3]) != "--warmup-runs" ||
			!parse_count(argv[4], warmup_runs)) {
			std::fprintf(stderr,
				"usage: %s [--benchmark-runs N --warmup-runs N]\n",
				argv[0]);
			return 2;
		}
		return run_benchmark(runs, warmup_runs);
	}
	std::vector<std::uint8_t> evidence;
	test_runtime_delta_contract(evidence);
	test_representative_workload(evidence, true);
	if (failure_count != 0) {
		std::fprintf(stderr, "M5_WORKLOAD_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M5_WORKLOAD_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"M5_WORKLOAD_PASS scenarios=6 viewers=2 moving_edits=8\n"
	);
	return 0;
}
