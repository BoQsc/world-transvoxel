#pragma once

#include "core/wt_chunk_state.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>

namespace world_transvoxel {

enum class WtChunkJobStage : std::uint8_t {
	Sample,
	Mesh,
};

struct WtChunkJob {
	WtChunkKey key;
	WtGenerationToken generation;
	std::uint64_t source_revision = 0;
	std::uint64_t world_revision = 0;
	std::uint64_t sequence = 0;
	std::int32_t priority = 0;
	WtChunkJobStage stage = WtChunkJobStage::Sample;
};

struct WtChunkJobResult {
	WtChunkKey key;
	WtGenerationToken generation;
	WtChunkJobStage stage = WtChunkJobStage::Sample;
	bool success = false;
};

enum class WtSchedulerStatus : std::uint8_t {
	Ok,
	AlreadyCurrent,
	NotFound,
	InvalidKey,
	RecordCapacityExceeded,
	JobQueueFull,
	CompletionQueueFull,
	ViewerCapacityExceeded,
	StaleViewerSnapshot,
};

struct WtSchedulerMetrics {
	std::uint64_t requested_jobs = 0;
	std::uint64_t completed_jobs = 0;
	std::uint64_t stale_results = 0;
	std::uint64_t cancellations = 0;
	std::uint64_t discarded_jobs = 0;
	std::uint64_t discarded_completions = 0;
	std::uint64_t queue_rejections = 0;
	std::uint64_t requested_records = 0;
	std::uint64_t sampling_records = 0;
	std::uint64_t meshing_records = 0;
	std::uint64_t ready_records = 0;
	std::uint64_t failed_records = 0;
};

class WtStreamScheduler {
public:
	WtStreamScheduler(
		std::size_t record_capacity,
		std::size_t job_capacity,
		std::size_t completion_capacity,
		std::size_t viewer_capacity
	);

	WtSchedulerStatus request_chunk(
		const WtChunkKey &key,
		std::uint64_t source_revision,
		std::int32_t priority
	);
	WtSchedulerStatus request_chunk_version(
		const WtChunkKey &key,
		std::uint64_t source_revision,
		std::uint64_t world_revision,
		std::int32_t priority,
		bool force_remesh = false
	);
	WtSchedulerStatus cancel_chunk(const WtChunkKey &key);
	WtSchedulerStatus forget_chunk(const WtChunkKey &key);
	WtSchedulerStatus reprioritize_chunk(
		const WtChunkKey &key,
		std::int32_t priority
	);
	bool pop_job(WtChunkJob &job);
	WtSchedulerStatus submit_completion(const WtChunkJobResult &result);
	std::size_t apply_completions(std::size_t maximum_count);
	WtSchedulerStatus update_viewer(const WtViewerSnapshot &snapshot);

	const WtChunkRecord *find_record(const WtChunkKey &key) const noexcept;
	const std::vector<WtChunkRecord> &get_records() const noexcept;
	const std::vector<WtViewerSnapshot> &get_viewers() const noexcept;
	WtSchedulerMetrics get_metrics() const noexcept;
	std::size_t record_capacity() const noexcept;
	std::size_t available_record_capacity() const noexcept;
	std::size_t queued_job_count() const noexcept;
	std::size_t available_job_capacity() const noexcept;
	std::size_t queued_completion_count() const noexcept;

private:
	class JobQueue {
	public:
		explicit JobQueue(std::size_t capacity);
		bool push(const WtChunkJob &job);
		bool pop(WtChunkJob &job);
		bool reprioritize(
			const WtChunkKey &key,
			WtGenerationToken generation,
			std::int32_t priority
		);
		std::size_t erase_key(const WtChunkKey &key);
		std::size_t size() const noexcept;
		std::size_t available() const noexcept;

	private:
		std::size_t capacity_ = 0;
		mutable std::mutex mutex_;
		std::vector<WtChunkJob> jobs_;
	};

	class CompletionQueue {
	public:
		explicit CompletionQueue(std::size_t capacity);
		bool push(const WtChunkJobResult &result);
		bool pop(WtChunkJobResult &result);
		std::size_t erase_key(const WtChunkKey &key);
		std::size_t size() const noexcept;

	private:
		std::size_t capacity_ = 0;
		std::size_t head_ = 0;
		std::size_t count_ = 0;
		mutable std::mutex mutex_;
		std::vector<WtChunkJobResult> slots_;
	};

	WtChunkRecord *find_record_mutable(const WtChunkKey &key) noexcept;
	WtGenerationToken next_generation() noexcept;
	WtChunkJob make_job(const WtChunkRecord &record, WtChunkJobStage stage) noexcept;

	std::size_t record_capacity_ = 0;
	std::size_t viewer_capacity_ = 0;
	std::uint64_t generation_counter_ = 0;
	std::uint64_t sequence_counter_ = 0;
	std::vector<WtChunkRecord> records_;
	std::vector<WtViewerSnapshot> viewers_;
	JobQueue jobs_;
	CompletionQueue completions_;
	WtSchedulerMetrics metrics_;
	std::atomic<std::uint64_t> asynchronous_queue_rejections_{ 0 };
};

} // namespace world_transvoxel
