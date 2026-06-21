#pragma once

#include "storage/wt_container_format.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

constexpr std::uint16_t kWtRuntimeTraceSchemaMajor = 1;
constexpr std::uint16_t kWtRuntimeTraceSchemaMinor = 0;
constexpr std::uint16_t kWtRuntimeTraceEventSize = 128;
constexpr std::size_t kWtRuntimeTraceMetadataSize = 80;
constexpr std::size_t kWtRuntimeTraceEventHeaderSize = 16;
constexpr std::size_t kWtMaximumRuntimeTraceEvents = 262144;
constexpr std::uint32_t kWtRuntimeTraceMetadataSection =
	wt_fourcc('M', 'E', 'T', 'A');
constexpr std::uint32_t kWtRuntimeTraceEventSection =
	wt_fourcc('E', 'V', 'N', 'T');

enum class WtRuntimeTraceEventKind : std::uint16_t {
	Checkpoint = 1,
	Final = 2,
};

struct WtRuntimeTraceEvent {
	std::uint64_t sequence = 0;
	std::uint64_t elapsed_ns = 0;
	std::uint64_t frame = 0;
	WtRuntimeTraceEventKind kind = WtRuntimeTraceEventKind::Checkpoint;
	std::uint16_t flags = 0;
	std::uint64_t desired_chunks = 0;
	std::uint64_t scheduler_records = 0;
	std::uint64_t queued_jobs = 0;
	std::uint64_t queued_completions = 0;
	std::uint64_t queued_render = 0;
	std::uint64_t queued_collision = 0;
	std::uint64_t resource_entries = 0;
	std::uint64_t pending_readiness = 0;
	std::uint64_t viewer_events = 0;
	std::uint64_t edit_events = 0;
	std::uint64_t stale_results = 0;
	std::uint64_t total_rejections = 0;

	bool operator==(const WtRuntimeTraceEvent &other) const noexcept;
};

struct WtRuntimeTraceConfig {
	std::uint64_t source_revision = 0;
	std::uint64_t target_duration_ns = 0;
	std::uint64_t sample_period_frames = 0;
	std::uint64_t seed = 0;
	std::size_t event_capacity = 0;
};

struct WtRuntimeTraceMetadata {
	std::uint64_t source_revision = 0;
	std::uint64_t target_duration_ns = 0;
	std::uint64_t actual_duration_ns = 0;
	std::uint64_t frame_count = 0;
	std::uint64_t event_count = 0;
	std::uint64_t event_capacity = 0;
	std::uint64_t sample_period_frames = 0;
	std::uint64_t seed = 0;
};

enum class WtRuntimeTraceStatus : std::uint8_t {
	Ok,
	InvalidConfiguration,
	InvalidEvent,
	CapacityExceeded,
	ContainerFailure,
	InvalidTrace,
};

class WtRuntimeTraceWriter {
public:
	explicit WtRuntimeTraceWriter(WtRuntimeTraceConfig config);

	bool valid() const noexcept;
	WtRuntimeTraceStatus append(WtRuntimeTraceEvent event);
	WtRuntimeTraceStatus finish(
		std::uint64_t actual_duration_ns,
		std::uint64_t frame_count,
		std::vector<std::uint8_t> &output
	) const;
	std::size_t event_count() const noexcept;
	std::size_t event_capacity() const noexcept;
	const std::vector<WtRuntimeTraceEvent> &events() const noexcept;

private:
	WtRuntimeTraceConfig config_;
	bool valid_ = false;
	bool final_seen_ = false;
	std::vector<WtRuntimeTraceEvent> events_;
};

struct WtRuntimeTraceView {
	WtContainerView container;
	WtRuntimeTraceMetadata metadata;
	std::vector<WtRuntimeTraceEvent> events;
};

WtRuntimeTraceStatus wt_open_runtime_trace(
	WtByteView bytes,
	WtRuntimeTraceView &output
);

} // namespace world_transvoxel
