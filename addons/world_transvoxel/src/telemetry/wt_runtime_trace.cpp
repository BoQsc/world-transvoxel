#include "telemetry/wt_runtime_trace.h"

#include "storage/wt_binary_io.h"

#include <utility>

namespace world_transvoxel {
namespace {

bool valid_kind(WtRuntimeTraceEventKind kind) noexcept {
	return kind == WtRuntimeTraceEventKind::Checkpoint ||
		kind == WtRuntimeTraceEventKind::Final;
}

bool valid_config(const WtRuntimeTraceConfig &config) noexcept {
	return config.source_revision != 0 &&
		config.target_duration_ns != 0 &&
		config.sample_period_frames != 0 &&
		config.event_capacity != 0 &&
		config.event_capacity <= kWtMaximumRuntimeTraceEvents;
}

bool write_event(
	WtBinaryWriter &writer,
	const WtRuntimeTraceEvent &event
) {
	return writer.write_u64(event.sequence) == WtBinaryStatus::Ok &&
		writer.write_u64(event.elapsed_ns) == WtBinaryStatus::Ok &&
		writer.write_u64(event.frame) == WtBinaryStatus::Ok &&
		writer.write_u16(static_cast<std::uint16_t>(event.kind)) ==
			WtBinaryStatus::Ok &&
		writer.write_u16(event.flags) == WtBinaryStatus::Ok &&
		writer.write_u32(0) == WtBinaryStatus::Ok &&
		writer.write_u64(event.desired_chunks) == WtBinaryStatus::Ok &&
		writer.write_u64(event.scheduler_records) == WtBinaryStatus::Ok &&
		writer.write_u64(event.queued_jobs) == WtBinaryStatus::Ok &&
		writer.write_u64(event.queued_completions) == WtBinaryStatus::Ok &&
		writer.write_u64(event.queued_render) == WtBinaryStatus::Ok &&
		writer.write_u64(event.queued_collision) == WtBinaryStatus::Ok &&
		writer.write_u64(event.resource_entries) == WtBinaryStatus::Ok &&
		writer.write_u64(event.pending_readiness) == WtBinaryStatus::Ok &&
		writer.write_u64(event.viewer_events) == WtBinaryStatus::Ok &&
		writer.write_u64(event.edit_events) == WtBinaryStatus::Ok &&
		writer.write_u64(event.stale_results) == WtBinaryStatus::Ok &&
		writer.write_u64(event.total_rejections) == WtBinaryStatus::Ok;
}

bool read_event(
	WtBinaryReader &reader,
	WtRuntimeTraceEvent &event
) {
	std::uint16_t kind = 0;
	std::uint32_t reserved = 0;
	if (reader.read_u64(event.sequence) != WtBinaryStatus::Ok ||
		reader.read_u64(event.elapsed_ns) != WtBinaryStatus::Ok ||
		reader.read_u64(event.frame) != WtBinaryStatus::Ok ||
		reader.read_u16(kind) != WtBinaryStatus::Ok ||
		reader.read_u16(event.flags) != WtBinaryStatus::Ok ||
		reader.read_u32(reserved) != WtBinaryStatus::Ok ||
		reader.read_u64(event.desired_chunks) != WtBinaryStatus::Ok ||
		reader.read_u64(event.scheduler_records) != WtBinaryStatus::Ok ||
		reader.read_u64(event.queued_jobs) != WtBinaryStatus::Ok ||
		reader.read_u64(event.queued_completions) != WtBinaryStatus::Ok ||
		reader.read_u64(event.queued_render) != WtBinaryStatus::Ok ||
		reader.read_u64(event.queued_collision) != WtBinaryStatus::Ok ||
		reader.read_u64(event.resource_entries) != WtBinaryStatus::Ok ||
		reader.read_u64(event.pending_readiness) != WtBinaryStatus::Ok ||
		reader.read_u64(event.viewer_events) != WtBinaryStatus::Ok ||
		reader.read_u64(event.edit_events) != WtBinaryStatus::Ok ||
		reader.read_u64(event.stale_results) != WtBinaryStatus::Ok ||
		reader.read_u64(event.total_rejections) != WtBinaryStatus::Ok) {
		return false;
	}
	event.kind = static_cast<WtRuntimeTraceEventKind>(kind);
	return reserved == 0 && event.flags == 0 && valid_kind(event.kind);
}

bool encode_metadata(
	const WtRuntimeTraceConfig &config,
	std::uint64_t actual_duration_ns,
	std::uint64_t frame_count,
	std::size_t event_count,
	std::vector<std::uint8_t> &output
) {
	WtBinaryWriter writer(kWtRuntimeTraceMetadataSize);
	if (writer.write_u16(kWtRuntimeTraceSchemaMajor) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtRuntimeTraceSchemaMinor) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtRuntimeTraceEventSize) != WtBinaryStatus::Ok ||
		writer.write_u16(0) != WtBinaryStatus::Ok ||
		writer.write_u64(config.source_revision) != WtBinaryStatus::Ok ||
		writer.write_u64(config.target_duration_ns) != WtBinaryStatus::Ok ||
		writer.write_u64(actual_duration_ns) != WtBinaryStatus::Ok ||
		writer.write_u64(frame_count) != WtBinaryStatus::Ok ||
		writer.write_u64(event_count) != WtBinaryStatus::Ok ||
		writer.write_u64(config.event_capacity) != WtBinaryStatus::Ok ||
		writer.write_u64(config.sample_period_frames) != WtBinaryStatus::Ok ||
		writer.write_u64(config.seed) != WtBinaryStatus::Ok ||
		writer.write_u64(0) != WtBinaryStatus::Ok) {
		return false;
	}
	output = writer.take_bytes();
	return output.size() == kWtRuntimeTraceMetadataSize;
}

bool encode_events(
	const std::vector<WtRuntimeTraceEvent> &events,
	std::vector<std::uint8_t> &output
) {
	const std::size_t size = kWtRuntimeTraceEventHeaderSize +
		events.size() * kWtRuntimeTraceEventSize;
	if (size > kWtMaximumSectionSize) return false;
	WtBinaryWriter writer(size);
	if (writer.write_u16(kWtRuntimeTraceSchemaMajor) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtRuntimeTraceSchemaMinor) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtRuntimeTraceEventSize) != WtBinaryStatus::Ok ||
		writer.write_u16(0) != WtBinaryStatus::Ok ||
		writer.write_u64(events.size()) != WtBinaryStatus::Ok) {
		return false;
	}
	for (const WtRuntimeTraceEvent &event : events) {
		if (!write_event(writer, event)) return false;
	}
	output = writer.take_bytes();
	return output.size() == size;
}

bool decode_metadata(
	WtByteView bytes,
	WtRuntimeTraceMetadata &output
) {
	if (bytes.size != kWtRuntimeTraceMetadataSize) return false;
	WtBinaryReader reader(bytes);
	std::uint16_t major = 0;
	std::uint16_t minor = 0;
	std::uint16_t event_size = 0;
	std::uint16_t reserved16 = 0;
	std::uint64_t reserved64 = 0;
	return reader.read_u16(major) == WtBinaryStatus::Ok &&
		reader.read_u16(minor) == WtBinaryStatus::Ok &&
		reader.read_u16(event_size) == WtBinaryStatus::Ok &&
		reader.read_u16(reserved16) == WtBinaryStatus::Ok &&
		reader.read_u64(output.source_revision) == WtBinaryStatus::Ok &&
		reader.read_u64(output.target_duration_ns) == WtBinaryStatus::Ok &&
		reader.read_u64(output.actual_duration_ns) == WtBinaryStatus::Ok &&
		reader.read_u64(output.frame_count) == WtBinaryStatus::Ok &&
		reader.read_u64(output.event_count) == WtBinaryStatus::Ok &&
		reader.read_u64(output.event_capacity) == WtBinaryStatus::Ok &&
		reader.read_u64(output.sample_period_frames) == WtBinaryStatus::Ok &&
		reader.read_u64(output.seed) == WtBinaryStatus::Ok &&
		reader.read_u64(reserved64) == WtBinaryStatus::Ok &&
		reader.remaining() == 0 &&
		major == kWtRuntimeTraceSchemaMajor &&
		minor <= kWtRuntimeTraceSchemaMinor &&
		event_size == kWtRuntimeTraceEventSize &&
		reserved16 == 0 && reserved64 == 0 &&
		output.source_revision != 0 &&
		output.target_duration_ns != 0 &&
		output.sample_period_frames != 0 &&
		output.event_capacity != 0 &&
		output.event_capacity <= kWtMaximumRuntimeTraceEvents &&
		output.event_count != 0 &&
		output.event_count <= output.event_capacity;
}

bool decode_events(
	WtByteView bytes,
	const WtRuntimeTraceMetadata &metadata,
	std::vector<WtRuntimeTraceEvent> &output
) {
	WtBinaryReader reader(bytes);
	std::uint16_t major = 0;
	std::uint16_t minor = 0;
	std::uint16_t event_size = 0;
	std::uint16_t reserved = 0;
	std::uint64_t count = 0;
	if (reader.read_u16(major) != WtBinaryStatus::Ok ||
		reader.read_u16(minor) != WtBinaryStatus::Ok ||
		reader.read_u16(event_size) != WtBinaryStatus::Ok ||
		reader.read_u16(reserved) != WtBinaryStatus::Ok ||
		reader.read_u64(count) != WtBinaryStatus::Ok ||
		major != kWtRuntimeTraceSchemaMajor ||
		minor > kWtRuntimeTraceSchemaMinor ||
		event_size != kWtRuntimeTraceEventSize ||
		reserved != 0 ||
		count != metadata.event_count ||
		count > kWtMaximumRuntimeTraceEvents ||
		bytes.size != kWtRuntimeTraceEventHeaderSize +
			static_cast<std::size_t>(count) * kWtRuntimeTraceEventSize) {
		return false;
	}
	output.reserve(static_cast<std::size_t>(count));
	for (std::uint64_t index = 0; index < count; ++index) {
		WtRuntimeTraceEvent event;
		if (!read_event(reader, event) ||
			event.sequence != index ||
			(!output.empty() &&
				(event.elapsed_ns < output.back().elapsed_ns ||
					event.frame < output.back().frame)) ||
			(event.kind == WtRuntimeTraceEventKind::Final &&
				index + 1 != count) ||
			(event.kind != WtRuntimeTraceEventKind::Final &&
				index + 1 == count)) {
			return false;
		}
		output.push_back(event);
	}
	return reader.remaining() == 0 &&
		output.back().elapsed_ns == metadata.actual_duration_ns &&
		output.back().frame == metadata.frame_count;
}

} // namespace

bool WtRuntimeTraceEvent::operator==(
	const WtRuntimeTraceEvent &other
) const noexcept {
	return sequence == other.sequence &&
		elapsed_ns == other.elapsed_ns &&
		frame == other.frame &&
		kind == other.kind &&
		flags == other.flags &&
		desired_chunks == other.desired_chunks &&
		scheduler_records == other.scheduler_records &&
		queued_jobs == other.queued_jobs &&
		queued_completions == other.queued_completions &&
		queued_render == other.queued_render &&
		queued_collision == other.queued_collision &&
		resource_entries == other.resource_entries &&
		pending_readiness == other.pending_readiness &&
		viewer_events == other.viewer_events &&
		edit_events == other.edit_events &&
		stale_results == other.stale_results &&
		total_rejections == other.total_rejections;
}

WtRuntimeTraceWriter::WtRuntimeTraceWriter(WtRuntimeTraceConfig config) :
		config_(config),
		valid_(valid_config(config)) {
	if (valid_) events_.reserve(config.event_capacity);
}

bool WtRuntimeTraceWriter::valid() const noexcept {
	return valid_;
}

WtRuntimeTraceStatus WtRuntimeTraceWriter::append(
	WtRuntimeTraceEvent event
) {
	if (!valid_ || final_seen_ || !valid_kind(event.kind) ||
		event.flags != 0) {
		return WtRuntimeTraceStatus::InvalidEvent;
	}
	if (events_.size() >= config_.event_capacity) {
		return WtRuntimeTraceStatus::CapacityExceeded;
	}
	if (!events_.empty() &&
		(event.elapsed_ns < events_.back().elapsed_ns ||
			event.frame < events_.back().frame)) {
		return WtRuntimeTraceStatus::InvalidEvent;
	}
	event.sequence = events_.size();
	final_seen_ = event.kind == WtRuntimeTraceEventKind::Final;
	events_.push_back(event);
	return WtRuntimeTraceStatus::Ok;
}

WtRuntimeTraceStatus WtRuntimeTraceWriter::finish(
	std::uint64_t actual_duration_ns,
	std::uint64_t frame_count,
	std::vector<std::uint8_t> &output
) const {
	output.clear();
	if (!valid_ || events_.empty() || !final_seen_ ||
		events_.back().elapsed_ns != actual_duration_ns ||
		events_.back().frame != frame_count) {
		return WtRuntimeTraceStatus::InvalidEvent;
	}
	std::vector<std::uint8_t> metadata;
	std::vector<std::uint8_t> events;
	if (!encode_metadata(
			config_,
			actual_duration_ns,
			frame_count,
			events_.size(),
			metadata
		) || !encode_events(events_, events)) {
		return WtRuntimeTraceStatus::CapacityExceeded;
	}
	const std::vector<WtContainerSectionInput> sections = {
		{
			kWtRuntimeTraceMetadataSection,
			0,
			WtStorageCodec::None,
			{ metadata.data(), metadata.size() },
		},
		{
			kWtRuntimeTraceEventSection,
			0,
			WtStorageCodec::None,
			{ events.data(), events.size() },
		},
	};
	return wt_write_container(
		kWtTraceMagic,
		0,
		config_.source_revision,
		sections,
		output
	) == WtContainerStatus::Ok ?
		WtRuntimeTraceStatus::Ok :
		WtRuntimeTraceStatus::ContainerFailure;
}

std::size_t WtRuntimeTraceWriter::event_count() const noexcept {
	return events_.size();
}

std::size_t WtRuntimeTraceWriter::event_capacity() const noexcept {
	return config_.event_capacity;
}

const std::vector<WtRuntimeTraceEvent> &
WtRuntimeTraceWriter::events() const noexcept {
	return events_;
}

WtRuntimeTraceStatus wt_open_runtime_trace(
	WtByteView bytes,
	WtRuntimeTraceView &output
) {
	output = {};
	WtRuntimeTraceView candidate;
	if (wt_read_container(bytes, kWtTraceMagic, candidate.container) !=
		WtContainerStatus::Ok) {
		return WtRuntimeTraceStatus::ContainerFailure;
	}
	const WtContainerSection *metadata =
		candidate.container.find_section(kWtRuntimeTraceMetadataSection);
	const WtContainerSection *events =
		candidate.container.find_section(kWtRuntimeTraceEventSection);
	if (metadata == nullptr || events == nullptr ||
		metadata->flags != 0 || events->flags != 0 ||
		!decode_metadata(metadata->payload, candidate.metadata) ||
		candidate.metadata.source_revision !=
			candidate.container.header.source_revision ||
		!decode_events(
			events->payload,
			candidate.metadata,
			candidate.events
		)) {
		return WtRuntimeTraceStatus::InvalidTrace;
	}
	output = std::move(candidate);
	return WtRuntimeTraceStatus::Ok;
}

} // namespace world_transvoxel
