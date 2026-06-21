#include "storage/wt_hash256.h"
#include "telemetry/wt_runtime_trace.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
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

wt::WtRuntimeTraceEvent event(
	wt::WtRuntimeTraceEventKind kind,
	std::uint64_t elapsed_ns,
	std::uint64_t frame,
	std::uint64_t base
) {
	wt::WtRuntimeTraceEvent result;
	result.kind = kind;
	result.elapsed_ns = elapsed_ns;
	result.frame = frame;
	result.desired_chunks = base + 1;
	result.scheduler_records = base + 2;
	result.queued_jobs = base + 3;
	result.queued_completions = base + 4;
	result.queued_render = base + 5;
	result.queued_collision = base + 6;
	result.resource_entries = base + 7;
	result.pending_readiness = base + 8;
	result.viewer_events = base + 9;
	result.edit_events = base + 10;
	result.stale_results = base + 11;
	result.total_rejections = base + 12;
	return result;
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void patch_u16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint16_t value) {
	bytes[offset] = static_cast<std::uint8_t>(value);
	bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}

void patch_u64(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint64_t value) {
	for (std::size_t index = 0; index < 8; ++index) {
		bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
	}
}

std::vector<std::uint8_t> rewrite_trace(
	const std::vector<std::uint8_t> &bytes,
	std::vector<std::uint8_t> metadata,
	bool add_optional_section
) {
	wt::WtContainerView container;
	check(wt::wt_read_container(
		{ bytes.data(), bytes.size() },
		wt::kWtTraceMagic,
		container
	) == wt::WtContainerStatus::Ok, "trace fixture container did not open");
	const wt::WtContainerSection *events =
		container.find_section(wt::kWtRuntimeTraceEventSection);
	check(events != nullptr, "trace fixture event section is missing");
	const std::uint8_t optional_payload[] = { 3, 1, 4, 1, 5 };
	std::vector<wt::WtContainerSectionInput> sections = {
		{
			wt::kWtRuntimeTraceMetadataSection,
			0,
			wt::WtStorageCodec::None,
			{ metadata.data(), metadata.size() },
		},
		{
			wt::kWtRuntimeTraceEventSection,
			0,
			wt::WtStorageCodec::None,
			events == nullptr ? wt::WtByteView{} : events->payload,
		},
	};
	if (add_optional_section) {
		sections.push_back({
			wt::wt_fourcc('N', 'O', 'T', 'E'),
			0,
			wt::WtStorageCodec::None,
			{ optional_payload, sizeof(optional_payload) },
		});
	}
	std::vector<std::uint8_t> output;
	check(wt::wt_write_container(
		wt::kWtTraceMagic,
		0,
		container.header.source_revision,
		sections,
		output
	) == wt::WtContainerStatus::Ok, "rewritten trace container failed");
	return output;
}

std::vector<std::uint8_t> test_round_trip() {
	const wt::WtRuntimeTraceConfig config = {
		700,
		300,
		10,
		0x123456789abcdef0ULL,
		4,
	};
	wt::WtRuntimeTraceWriter writer(config);
	check(writer.valid(), "valid trace writer configuration was rejected");
	const wt::WtRuntimeTraceEvent first =
		event(wt::WtRuntimeTraceEventKind::Checkpoint, 100, 10, 20);
	const wt::WtRuntimeTraceEvent second =
		event(wt::WtRuntimeTraceEventKind::Checkpoint, 200, 20, 40);
	const wt::WtRuntimeTraceEvent final =
		event(wt::WtRuntimeTraceEventKind::Final, 300, 25, 60);
	check(writer.append(first) == wt::WtRuntimeTraceStatus::Ok,
		"first trace event failed");
	check(writer.append(second) == wt::WtRuntimeTraceStatus::Ok,
		"second trace event failed");
	check(writer.append(final) == wt::WtRuntimeTraceStatus::Ok,
		"final trace event failed");
	check(writer.event_count() == 3 && writer.event_capacity() == 4,
		"trace writer counts mismatch");

	std::vector<std::uint8_t> bytes;
	check(writer.finish(300, 25, bytes) == wt::WtRuntimeTraceStatus::Ok,
		"trace encoding failed");
	wt::WtRuntimeTraceView view;
	check(wt::wt_open_runtime_trace(
		{ bytes.data(), bytes.size() },
		view
	) == wt::WtRuntimeTraceStatus::Ok, "encoded trace did not open");
	check(view.metadata.source_revision == config.source_revision &&
		view.metadata.target_duration_ns == config.target_duration_ns &&
		view.metadata.actual_duration_ns == 300 &&
		view.metadata.frame_count == 25 &&
		view.metadata.event_count == 3 &&
		view.metadata.event_capacity == 4 &&
		view.metadata.sample_period_frames == 10 &&
		view.metadata.seed == config.seed,
		"trace metadata round trip mismatch");
	check(view.events.size() == 3 &&
		view.events[0].sequence == 0 &&
		view.events[1].sequence == 1 &&
		view.events[2].sequence == 2 &&
		view.events[0] == writer.events()[0] &&
		view.events[1] == writer.events()[1] &&
		view.events[2] == writer.events()[2],
		"trace event round trip mismatch");
	return bytes;
}

void test_rejections(const std::vector<std::uint8_t> &valid_bytes) {
	wt::WtRuntimeTraceWriter invalid({});
	check(!invalid.valid(), "invalid trace writer configuration was accepted");

	wt::WtRuntimeTraceWriter limited({ 1, 10, 1, 0, 1 });
	check(limited.append(event(
		wt::WtRuntimeTraceEventKind::Checkpoint, 1, 1, 0
	)) == wt::WtRuntimeTraceStatus::Ok, "limited trace first event failed");
	check(limited.append(event(
		wt::WtRuntimeTraceEventKind::Final, 2, 2, 0
	)) == wt::WtRuntimeTraceStatus::CapacityExceeded,
		"trace event capacity overflow was accepted");
	std::vector<std::uint8_t> output;
	check(limited.finish(1, 1, output) == wt::WtRuntimeTraceStatus::InvalidEvent,
		"trace without a final event was encoded");

	wt::WtRuntimeTraceWriter ordered({ 1, 10, 1, 0, 3 });
	check(ordered.append(event(
		wt::WtRuntimeTraceEventKind::Checkpoint, 5, 5, 0
	)) == wt::WtRuntimeTraceStatus::Ok, "ordered trace first event failed");
	check(ordered.append(event(
		wt::WtRuntimeTraceEventKind::Checkpoint, 4, 6, 0
	)) == wt::WtRuntimeTraceStatus::InvalidEvent,
		"decreasing trace time was accepted");
	check(ordered.append(event(
		wt::WtRuntimeTraceEventKind::Final, 6, 6, 0
	)) == wt::WtRuntimeTraceStatus::Ok, "ordered trace final event failed");
	check(ordered.append(event(
		wt::WtRuntimeTraceEventKind::Checkpoint, 7, 7, 0
	)) == wt::WtRuntimeTraceStatus::InvalidEvent,
		"event after final trace event was accepted");

	std::vector<std::uint8_t> corrupted = valid_bytes;
	corrupted.back() ^= 0x80U;
	wt::WtRuntimeTraceView view;
	check(wt::wt_open_runtime_trace(
		{ corrupted.data(), corrupted.size() },
		view
	) == wt::WtRuntimeTraceStatus::ContainerFailure,
		"corrupt trace payload was accepted");

	wt::WtContainerView container;
	check(wt::wt_read_container(
		{ valid_bytes.data(), valid_bytes.size() },
		wt::kWtTraceMagic,
		container
	) == wt::WtContainerStatus::Ok, "valid trace container did not reopen");
	const wt::WtContainerSection *metadata_section =
		container.find_section(wt::kWtRuntimeTraceMetadataSection);
	check(metadata_section != nullptr, "valid trace metadata is missing");
	if (metadata_section == nullptr) return;
	std::vector<std::uint8_t> metadata(
		metadata_section->payload.data,
		metadata_section->payload.data + metadata_section->payload.size
	);

	std::vector<std::uint8_t> future_metadata = metadata;
	patch_u16(future_metadata, 0, 2);
	std::vector<std::uint8_t> future =
		rewrite_trace(valid_bytes, future_metadata, false);
	check(wt::wt_open_runtime_trace(
		{ future.data(), future.size() },
		view
	) == wt::WtRuntimeTraceStatus::InvalidTrace,
		"unsupported trace schema was accepted");

	std::vector<std::uint8_t> mismatched_metadata = metadata;
	patch_u64(mismatched_metadata, 8, 701);
	std::vector<std::uint8_t> mismatched =
		rewrite_trace(valid_bytes, mismatched_metadata, false);
	check(wt::wt_open_runtime_trace(
		{ mismatched.data(), mismatched.size() },
		view
	) == wt::WtRuntimeTraceStatus::InvalidTrace,
		"mismatched trace source revision was accepted");

	std::vector<std::uint8_t> extended =
		rewrite_trace(valid_bytes, metadata, true);
	check(wt::wt_open_runtime_trace(
		{ extended.data(), extended.size() },
		view
	) == wt::WtRuntimeTraceStatus::Ok,
		"optional trace section was not skipped");
}

} // namespace

int main() {
	const std::vector<std::uint8_t> bytes = test_round_trip();
	test_rejections(bytes);
	if (failure_count != 0) {
		std::fprintf(stderr, "M5_RUNTIME_TRACE_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M5_RUNTIME_TRACE_HASH ");
	print_hash(wt::wt_sha256(bytes.data(), bytes.size()));
	std::printf(
		"M5_RUNTIME_TRACE_PASS schema=1 event_size=128 events=3\n"
	);
	return 0;
}
