#include "telemetry/wt_runtime_trace.h"
#include "wt_m5_workload_fixture.h"

#include "storage/wt_hash256.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace wt = world_transvoxel;
namespace wtt = world_transvoxel::testing;

namespace {

using Clock = std::chrono::steady_clock;

enum class Axis {
	X,
	Y,
	Z,
};

struct Position {
	std::int32_t x = 0;
	std::int32_t y = 0;
	std::int32_t z = 0;
	Axis axis = Axis::X;
};

std::vector<wt::WtViewerChunkDemand> line_demands(
	const Position &position
) {
	std::vector<wt::WtViewerChunkDemand> demands;
	for (std::int32_t offset = -2; offset <= 2; ++offset) {
		wt::WtChunkKey key = {
			position.x,
			position.y,
			position.z,
			0,
		};
		if (position.axis == Axis::X) key.x += offset;
		else if (position.axis == Axis::Y) key.y += offset;
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
	const Position &position
) {
	return {
		id,
		static_cast<double>(position.x) * 16.0 + 8.0,
		static_cast<double>(position.y) * 16.0 + 8.0,
		static_cast<double>(position.z) * 16.0 + 8.0,
		revision,
	};
}

wt::WtGridPoint chunk_center(const Position &position) {
	return {
		static_cast<std::int64_t>(position.x) * 16 + 8,
		static_cast<std::int64_t>(position.y) * 16 + 8,
		static_cast<std::int64_t>(position.z) * 16 + 8,
	};
}

Position primary_position(std::uint64_t frame) {
	const std::int32_t cycle = static_cast<std::int32_t>(frame % 2048);
	const std::int32_t segment = cycle / 256;
	const std::int32_t step = cycle % 256;
	switch (segment) {
		case 0: return { step, 0, 0, Axis::X };
		case 1: return { 256 + step * 2, 0, 0, Axis::X };
		case 2: return { 1024, -64 - step, 0, Axis::Y };
		case 3: return { 1024, 64 + step * 2, 0, Axis::Y };
		case 4: return { 2048, 0, step, Axis::Z };
		case 5: return { -2048 + step, 32, 512, Axis::X };
		case 6: return { step, step / 4, -step, Axis::Z };
		default: return { 0, 0, 1024 - step * 2, Axis::Z };
	}
}

Position secondary_position(std::uint64_t frame) {
	const std::int32_t step = static_cast<std::int32_t>(frame % 512);
	return { 4096 - step, 96 + step / 8, -1024 + step, Axis::Z };
}

bool secondary_active(std::uint64_t frame) {
	const std::uint64_t segment = (frame % 2048) / 256;
	return segment >= 4 && segment <= 6;
}

std::uint64_t total_rejections(const wtt::WtM5WorkloadFixture &fixture) {
	const wt::WtMultiViewerDesiredSetMetrics desired =
		fixture.desired_set().get_metrics();
	const wt::WtSchedulerMetrics scheduler =
		fixture.scheduler().get_metrics();
	const wt::WtApplicationMetrics application =
		fixture.application().get_metrics();
	const wt::WtDesiredSetRuntimeMetrics runtime =
		fixture.runtime_service().get_metrics();
	const wt::WtEditRuntimeReplacementMetrics edits =
		fixture.edit_service().get_metrics();
	return desired.rejected_events +
		scheduler.queue_rejections +
		application.queue_rejections +
		application.sink_failures +
		runtime.capacity_rejections +
		runtime.state_rejections +
		runtime.scheduler_failures +
		runtime.application_failures +
		runtime.page_meshing_runtime_failures +
		edits.spatial_rejections +
		edits.capacity_rejections +
		edits.state_rejections +
		edits.scheduler_failures +
		edits.application_failures +
		edits.page_meshing_runtime_failures;
}

wt::WtRuntimeTraceEvent trace_event(
	const wtt::WtM5WorkloadFixture &fixture,
	wt::WtRuntimeTraceEventKind kind,
	std::uint64_t elapsed_ns
) {
	const wtt::WtM5WorkloadMetrics metrics = fixture.get_metrics();
	const wt::WtSchedulerMetrics scheduler =
		fixture.scheduler().get_metrics();
	wt::WtRuntimeTraceEvent event;
	event.elapsed_ns = elapsed_ns;
	event.frame = metrics.frames;
	event.kind = kind;
	event.desired_chunks =
		fixture.desired_set().get_desired_chunks().size();
	event.scheduler_records = fixture.scheduler().get_records().size();
	event.queued_jobs = fixture.scheduler().queued_job_count();
	event.queued_completions = fixture.scheduler().queued_completion_count();
	event.queued_render = fixture.application().queued_render_count();
	event.queued_collision = fixture.application().queued_collision_count();
	event.resource_entries =
		fixture.resource_cache().mesh_entry_count() +
		fixture.resource_cache().render_entry_count() +
		fixture.resource_cache().collision_entry_count();
	event.pending_readiness = fixture.pending_readiness_count();
	event.viewer_events = metrics.viewer_events;
	event.edit_events = metrics.edit_events;
	event.stale_results = scheduler.stale_results;
	event.total_rejections = total_rejections(fixture);
	return event;
}

bool parse_u64(const char *text, std::uint64_t &output) {
	char *end = nullptr;
	const unsigned long long value = std::strtoull(text, &end, 10);
	if (end == text || *end != '\0') return false;
	output = static_cast<std::uint64_t>(value);
	return true;
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

bool write_trace(
	const std::string &path,
	const std::vector<std::uint8_t> &bytes
) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) return false;
	output.write(
		reinterpret_cast<const char *>(bytes.data()),
		static_cast<std::streamsize>(bytes.size())
	);
	return output.good();
}

bool final_state_valid(const wtt::WtM5WorkloadFixture &fixture) {
	if (fixture.desired_set().get_desired_chunks().size() !=
			fixture.scheduler().get_records().size() ||
		fixture.scheduler().get_records().size() !=
			fixture.application().get_records().size() ||
		fixture.pending_readiness_count() != 0) {
		return false;
	}
	for (const wt::WtChunkApplicationRecord &record :
		fixture.application().get_records()) {
		if (!record.fully_ready()) return false;
	}
	return true;
}

bool metrics_valid(const wtt::WtM5WorkloadFixture &fixture) {
	const wtt::WtM5WorkloadMetrics metrics = fixture.get_metrics();
	const wt::WtSchedulerMetrics scheduler =
		fixture.scheduler().get_metrics();
	const wt::WtApplicationMetrics application =
		fixture.application().get_metrics();
	const wt::WtDesiredSetRuntimeMetrics runtime =
		fixture.runtime_service().get_metrics();
	const wt::WtEditRuntimeReplacementMetrics edits =
		fixture.edit_service().get_metrics();
	return total_rejections(fixture) == 0 &&
		metrics.maximum_desired_chunks <= 10 &&
		metrics.maximum_scheduler_records <= 10 &&
		metrics.maximum_job_queue < 128 &&
		metrics.maximum_completion_queue < 128 &&
		metrics.maximum_render_queue < 128 &&
		metrics.maximum_collision_queue < 128 &&
		metrics.maximum_resource_entries <= 30 &&
		metrics.maximum_pending_readiness <= 10 &&
		metrics.maximum_readiness_latency_frames <= 32 &&
		metrics.viewer_events > 0 &&
		metrics.edit_events > 0 &&
		scheduler.cancellations > 0 &&
		scheduler.stale_results > 0 &&
		application.applied_render > 0 &&
		application.applied_collision > 0 &&
		runtime.added_chunks > 0 &&
		runtime.removed_chunks > 0 &&
		runtime.updated_chunks > 0 &&
		edits.replaced_chunks > 0;
}

int run_soak(
	std::uint64_t duration_ms,
	std::uint64_t sample_period_frames,
	const std::string &trace_path
) {
	if (duration_ms == 0 || duration_ms > 3600000 ||
		sample_period_frames == 0) {
		std::fprintf(stderr, "M5_SOAK_FAIL invalid_limits\n");
		return 2;
	}
	const std::uint64_t target_ns = duration_ms * 1000000ULL;
	wtt::WtM5WorkloadFixture fixture;
	wt::WtRuntimeTraceWriter trace({
		700,
		target_ns,
		sample_period_frames,
		0x4d35534f414b0001ULL,
		65536,
	});
	if (!fixture.valid() || !trace.valid() ||
		trace.append(trace_event(
			fixture,
			wt::WtRuntimeTraceEventKind::Checkpoint,
			0
		)) != wt::WtRuntimeTraceStatus::Ok) {
		std::fprintf(stderr, "M5_SOAK_FAIL invalid_fixture\n");
		return 1;
	}

	std::uint64_t primary_revision = 0;
	std::uint64_t secondary_revision = 0;
	bool secondary_present = false;
	std::uint64_t maximum_frame_ns = 0;
	const auto start = Clock::now();
	std::uint64_t elapsed_ns = 0;
	while (elapsed_ns < target_ns) {
		const auto frame_start = Clock::now();
		const std::uint64_t frame = fixture.get_metrics().frames;
		const Position primary = primary_position(frame);
		++primary_revision;
		if (!fixture.update_viewer(
			viewer(1, primary_revision, primary),
			line_demands(primary)
		)) {
			std::fprintf(stderr, "M5_SOAK_FAIL primary_viewer frame=%llu\n",
				static_cast<unsigned long long>(frame));
			return 1;
		}
		const bool want_secondary = secondary_active(frame);
		if (want_secondary) {
			const Position secondary = secondary_position(frame);
			++secondary_revision;
			if (!fixture.update_viewer(
				viewer(2, secondary_revision, secondary),
				line_demands(secondary)
			)) {
				std::fprintf(stderr,
					"M5_SOAK_FAIL secondary_viewer frame=%llu\n",
					static_cast<unsigned long long>(frame));
				return 1;
			}
			secondary_present = true;
		} else if (secondary_present) {
			++secondary_revision;
			if (!fixture.remove_viewer(2, secondary_revision)) {
				std::fprintf(stderr,
					"M5_SOAK_FAIL secondary_remove frame=%llu\n",
					static_cast<unsigned long long>(frame));
				return 1;
			}
			secondary_present = false;
		}
		if ((frame % 64) == 0 && !fixture.apply_edit(chunk_center(primary))) {
			std::fprintf(stderr, "M5_SOAK_FAIL edit frame=%llu\n",
				static_cast<unsigned long long>(frame));
			return 1;
		}
		if (!fixture.run_frame(8, 4, 2)) {
			std::fprintf(stderr, "M5_SOAK_FAIL frame=%llu\n",
				static_cast<unsigned long long>(frame));
			return 1;
		}
		const auto now = Clock::now();
		const std::uint64_t frame_ns =
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					now - frame_start
				).count()
			);
		maximum_frame_ns = std::max(maximum_frame_ns, frame_ns);
		elapsed_ns = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				now - start
			).count()
		);
		if ((fixture.get_metrics().frames % sample_period_frames) == 0 &&
			trace.append(trace_event(
				fixture,
				wt::WtRuntimeTraceEventKind::Checkpoint,
				elapsed_ns
			)) != wt::WtRuntimeTraceStatus::Ok) {
			std::fprintf(stderr, "M5_SOAK_FAIL trace_capacity\n");
			return 1;
		}
	}

	if (secondary_present) {
		++secondary_revision;
		if (!fixture.remove_viewer(2, secondary_revision)) {
			std::fprintf(stderr, "M5_SOAK_FAIL final_secondary_remove\n");
			return 1;
		}
	}
	if (!fixture.drain(256)) {
		std::fprintf(stderr, "M5_SOAK_FAIL final_drain\n");
		return 1;
	}
	elapsed_ns = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			Clock::now() - start
		).count()
	);
	if (!final_state_valid(fixture) || !metrics_valid(fixture) ||
		elapsed_ns < target_ns ||
		trace.append(trace_event(
			fixture,
			wt::WtRuntimeTraceEventKind::Final,
			elapsed_ns
		)) != wt::WtRuntimeTraceStatus::Ok) {
		std::fprintf(stderr, "M5_SOAK_FAIL acceptance\n");
		return 1;
	}

	std::vector<std::uint8_t> trace_bytes;
	const std::uint64_t frame_count = fixture.get_metrics().frames;
	if (trace.finish(elapsed_ns, frame_count, trace_bytes) !=
			wt::WtRuntimeTraceStatus::Ok) {
		std::fprintf(stderr, "M5_SOAK_FAIL trace_encode\n");
		return 1;
	}
	wt::WtRuntimeTraceView trace_view;
	if (wt::wt_open_runtime_trace(
		{ trace_bytes.data(), trace_bytes.size() },
		trace_view
	) != wt::WtRuntimeTraceStatus::Ok ||
		trace_view.events.size() != trace.event_count() ||
		!write_trace(trace_path, trace_bytes)) {
		std::fprintf(stderr, "M5_SOAK_FAIL trace_verify\n");
		return 1;
	}

	const wtt::WtM5WorkloadMetrics metrics = fixture.get_metrics();
	const wt::WtSchedulerMetrics scheduler =
		fixture.scheduler().get_metrics();
	const wt::WtApplicationMetrics application =
		fixture.application().get_metrics();
	std::printf(
		"M5_SOAK_METRICS duration_ns=%llu target_ns=%llu frames=%llu "
		"viewer_events=%llu edit_events=%llu worker_jobs=%llu "
		"desired_max=%zu records_max=%zu jobs_max=%zu completions_max=%zu "
		"render_max=%zu collision_max=%zu resources_max=%zu pending_max=%zu "
		"readiness_max=%llu stale=%llu cancellations=%llu "
		"applied_render=%llu applied_collision=%llu rejections=%llu "
		"frame_max_ns=%llu trace_events=%zu trace_bytes=%zu\n",
		static_cast<unsigned long long>(elapsed_ns),
		static_cast<unsigned long long>(target_ns),
		static_cast<unsigned long long>(metrics.frames),
		static_cast<unsigned long long>(metrics.viewer_events),
		static_cast<unsigned long long>(metrics.edit_events),
		static_cast<unsigned long long>(metrics.worker_jobs),
		metrics.maximum_desired_chunks,
		metrics.maximum_scheduler_records,
		metrics.maximum_job_queue,
		metrics.maximum_completion_queue,
		metrics.maximum_render_queue,
		metrics.maximum_collision_queue,
		metrics.maximum_resource_entries,
		metrics.maximum_pending_readiness,
		static_cast<unsigned long long>(
			metrics.maximum_readiness_latency_frames
		),
		static_cast<unsigned long long>(scheduler.stale_results),
		static_cast<unsigned long long>(scheduler.cancellations),
		static_cast<unsigned long long>(application.applied_render),
		static_cast<unsigned long long>(application.applied_collision),
		static_cast<unsigned long long>(total_rejections(fixture)),
		static_cast<unsigned long long>(maximum_frame_ns),
		trace.event_count(),
		trace_bytes.size()
	);
	std::printf("M5_SOAK_TRACE_SHA256 ");
	print_hash(wt::wt_sha256(trace_bytes.data(), trace_bytes.size()));
	std::printf("M5_SOAK_PASS\n");
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	std::uint64_t duration_ms = 0;
	std::uint64_t sample_period_frames = 0;
	std::string trace_path;
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--duration-ms" && index + 1 < argc) {
			if (!parse_u64(argv[++index], duration_ms)) duration_ms = 0;
		} else if (argument == "--sample-period-frames" &&
				index + 1 < argc) {
			if (!parse_u64(argv[++index], sample_period_frames)) {
				sample_period_frames = 0;
			}
		} else if (argument == "--trace" && index + 1 < argc) {
			trace_path = argv[++index];
		} else {
			std::fprintf(stderr,
				"usage: %s --duration-ms N --sample-period-frames N "
				"--trace PATH\n",
				argv[0]);
			return 2;
		}
	}
	if (duration_ms == 0 || sample_period_frames == 0 ||
		trace_path.empty()) {
		std::fprintf(stderr,
			"usage: %s --duration-ms N --sample-period-frames N "
			"--trace PATH\n",
			argv[0]);
		return 2;
	}
	return run_soak(duration_ms, sample_period_frames, trace_path);
}
