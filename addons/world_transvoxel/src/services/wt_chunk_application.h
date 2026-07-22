#pragma once

#include "core/wt_application_status.h"
#include "physics/wt_collision_apply_queue.h"
#include "render/wt_render_apply_queue.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

struct WtChunkApplicationRecord {
	WtChunkKey key;
	WtGenerationToken generation;
	bool collision_required = false;
	bool visual_required = true;
	bool visual_ready = false;
	bool collision_ready = false;
	bool staged_replacement = false;

	bool fully_ready() const noexcept;
};

struct WtApplicationMetrics {
	std::uint64_t submitted_render = 0;
	std::uint64_t submitted_collision = 0;
	std::uint64_t applied_render = 0;
	std::uint64_t applied_collision = 0;
	std::uint64_t stale_render = 0;
	std::uint64_t stale_collision = 0;
	std::uint64_t unrequired_collision = 0;
	std::uint64_t sink_failures = 0;
	std::uint64_t queue_rejections = 0;
	std::uint64_t render_latency_frames_total = 0;
	std::uint64_t render_latency_frames_maximum = 0;
	std::uint64_t collision_latency_frames_total = 0;
	std::uint64_t collision_latency_frames_maximum = 0;
};

struct WtApplicationBatchResult {
	std::size_t render_processed = 0;
	std::size_t collision_processed = 0;
};

class WtChunkApplicationService {
public:
	WtChunkApplicationService(
		std::size_t record_capacity,
		std::size_t render_queue_capacity,
		std::size_t collision_queue_capacity
	);

	WtApplicationStatus expect_chunk(
		const WtChunkKey &key,
		WtGenerationToken generation,
		bool collision_required,
		bool visual_required = true,
		bool staged_replacement = false,
		bool preserve_collision_ready = false
	);
	WtApplicationStatus set_visual_required(
		const WtChunkKey &key,
		bool required
	);
	WtApplicationStatus set_collision_required(
		const WtChunkKey &key,
		bool required
	);
	WtApplicationStatus forget_chunk(const WtChunkKey &key);
	WtApplicationStatus submit_render(const WtRenderPayloadPtr &payload);
	WtApplicationStatus submit_collision(const WtCollisionPayloadPtr &payload);
	WtApplicationBatchResult apply(
		std::size_t render_budget,
		std::size_t collision_budget,
		WtRenderSink &render_sink,
		WtCollisionSink &collision_sink
	);

	const WtChunkApplicationRecord *find_record(const WtChunkKey &key) const noexcept;
	const std::vector<WtChunkApplicationRecord> &get_records() const noexcept;
	WtApplicationMetrics get_metrics() const noexcept;
	std::size_t record_capacity() const noexcept;
	std::size_t available_record_capacity() const noexcept;
	std::size_t queued_render_count() const noexcept;
	std::size_t queued_collision_count() const noexcept;

private:
	WtChunkApplicationRecord *find_record_mutable(const WtChunkKey &key) noexcept;
	std::size_t apply_render(
		std::size_t budget,
		WtRenderSink &sink,
		std::uint64_t application_tick
	);
	std::size_t apply_deferred_collisions(
		std::size_t budget,
		WtCollisionSink &sink,
		std::uint64_t application_tick
	);
	std::size_t apply_collision(
		std::size_t budget,
		WtCollisionSink &sink,
		std::uint64_t application_tick
	);
	bool should_defer_collision(
		const WtChunkApplicationRecord &record
	) const noexcept;
	bool defer_collision(const WtCollisionApplyEntry &entry);

	std::size_t record_capacity_ = 0;
	std::vector<WtChunkApplicationRecord> records_;
	std::vector<WtCollisionApplyEntry> deferred_collisions_;
	WtRenderApplyQueue render_queue_;
	WtCollisionApplyQueue collision_queue_;
	WtApplicationMetrics metrics_;
	std::atomic<std::uint64_t> asynchronous_render_submissions_{ 0 };
	std::atomic<std::uint64_t> asynchronous_collision_submissions_{ 0 };
	std::atomic<std::uint64_t> asynchronous_queue_rejections_{ 0 };
	std::atomic<std::uint64_t> application_tick_{ 0 };
};

} // namespace world_transvoxel
