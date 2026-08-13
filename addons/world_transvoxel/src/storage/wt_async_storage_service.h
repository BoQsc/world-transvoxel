#pragma once

#include "core/wt_chunk_state.h"
#include "storage/wt_page_hierarchy.h"
#include "storage/wt_procedural_world_descriptor.h"
#include "storage/wt_world_manifest.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace world_transvoxel {

constexpr std::size_t kWtMaximumStorageQueueCapacity = 65536;
constexpr std::size_t kWtMaximumProceduralGenerationWorkerCount = 8;

struct WtScalarSample;

struct WtAsyncStorageLimits {
	std::size_t request_capacity = 256;
	std::size_t completion_capacity = 256;
	std::size_t maximum_page_bytes = kWtMaximumContainerSize;
	// File-backed storage always uses one I/O worker. This limit applies only
	// when open_procedural() generates immutable source pages.
	std::size_t procedural_generation_worker_count = 1;
};

enum class WtAsyncStorageStatus : std::uint8_t {
	Ok,
	InvalidConfiguration,
	AlreadyOpen,
	NotOpen,
	InvalidPath,
	ManifestIoFailure,
	ManifestFailure,
	PageSizeLimitExceeded,
	InvalidKey,
	PageNotFound,
	AlreadyPending,
	RequestQueueFull,
};

enum class WtPageLoadStatus : std::uint8_t {
	Ok,
	ObjectMissing,
	IoFailure,
	SizeMismatch,
	HashMismatch,
	PageFailure,
	MetadataMismatch,
	AllocationFailure,
};

struct WtPageLoadCompletion {
	WtChunkKey key;
	WtGenerationToken generation;
	WtPageLoadStatus status = WtPageLoadStatus::IoFailure;
	std::shared_ptr<const std::vector<std::uint8_t>> page_bytes;
};

struct WtAsyncStorageMetrics {
	std::uint64_t accepted_requests = 0;
	std::uint64_t started_requests = 0;
	std::uint64_t completed_requests = 0;
	std::uint64_t successful_pages = 0;
	std::uint64_t failed_pages = 0;
	std::uint64_t bytes_read = 0;
	std::uint64_t request_queue_rejections = 0;
	std::uint64_t duplicate_requests = 0;
	std::uint64_t cancelled_requests = 0;
	std::uint64_t discarded_completions = 0;
	std::uint64_t load_time_ns_last = 0;
	std::uint64_t load_time_ns_total = 0;
	std::uint64_t load_time_ns_maximum = 0;
	std::uint64_t worker_count = 0;
	std::uint64_t in_flight_requests = 0;
	std::uint64_t maximum_in_flight_requests = 0;
	std::uint64_t in_flight_elapsed_ns = 0;
	std::int64_t in_flight_key_x = 0;
	std::int64_t in_flight_key_y = 0;
	std::int64_t in_flight_key_z = 0;
	std::uint64_t in_flight_key_lod = 0;
	std::uint64_t in_flight_generation = 0;
};

enum class WtAsyncStorageTraceEventKind : std::uint8_t {
	Requested,
	Started,
	Finished,
};

using WtAsyncStorageTraceObserver = std::function<void(
	WtAsyncStorageTraceEventKind,
	const WtChunkKey &,
	WtGenerationToken,
	std::uint64_t,
	WtPageLoadStatus
)>;

std::filesystem::path wt_page_object_path(
	const std::filesystem::path &object_root,
	const WtHash256 &content_hash
);

class WtAsyncStorageService {
public:
	explicit WtAsyncStorageService(WtAsyncStorageLimits limits);
	~WtAsyncStorageService();

	WtAsyncStorageService(const WtAsyncStorageService &) = delete;
	WtAsyncStorageService &operator=(const WtAsyncStorageService &) = delete;

	WtAsyncStorageStatus open(
		const std::filesystem::path &world_manifest_path,
		const std::filesystem::path &object_root
	);
	WtAsyncStorageStatus open_procedural(
		const WtProceduralWorldDescriptor &descriptor
	);
	WtAsyncStorageStatus open_procedural_snapshot(
		const std::filesystem::path &snapshot_directory
	);
	void close() noexcept;

	WtAsyncStorageStatus request_page(
		const WtChunkKey &key,
		WtGenerationToken generation,
		std::int32_t priority
	);
	WtPageLoadStatus load_page_now(
		const WtChunkKey &key,
		std::shared_ptr<const std::vector<std::uint8_t>> &page_bytes
	) const;
	bool snapshot_manifest(
		std::vector<std::uint8_t> &manifest_bytes
	) const;
	bool sample_procedural_base(
		const WtGridPoint &point,
		WtScalarSample &output
	) const noexcept;
	bool pop_completion(WtPageLoadCompletion &completion);
	bool wait_pop_completion(
		WtPageLoadCompletion &completion,
		std::chrono::milliseconds timeout
	);
	void set_completion_notifier(std::function<void()> notifier);
	void set_trace_observer(WtAsyncStorageTraceObserver observer);

	bool is_open() const noexcept;
	bool is_procedural() const noexcept;
	bool procedural_descriptor(
		WtProceduralWorldDescriptor &descriptor
	) const noexcept;
	bool has_page(const WtChunkKey &key) const noexcept;
	bool has_overlay_page(const WtChunkKey &key) const noexcept;
	std::vector<WtChunkKey> page_keys() const;
	WtPageHierarchy page_hierarchy() const;
	std::uint64_t source_revision() const noexcept;
	std::uint64_t world_revision() const noexcept;
	std::size_t page_count() const noexcept;
	std::size_t overlay_page_count() const noexcept;
	std::size_t overlay_index_bytes() const noexcept;
	std::size_t queued_request_count() const noexcept;
	std::size_t queued_completion_count() const noexcept;
	std::size_t active_request_count() const noexcept;
	WtAsyncStorageMetrics get_metrics() const noexcept;

private:
	struct Request {
		WtChunkKey key;
		WtGenerationToken generation;
		WtWorldPageIndexEntry entry;
		bool procedural_fallback = false;
		std::uint64_t sequence = 0;
		std::int32_t priority = 0;
	};

	struct RequestIdentity {
		WtChunkKey key;
		WtGenerationToken generation;
	};

	struct InFlightRequest {
		RequestIdentity identity;
		std::chrono::steady_clock::time_point started;
	};

	bool configuration_valid() const noexcept;
	bool pop_completion_locked(WtPageLoadCompletion &completion);
	void remove_active_locked(const WtChunkKey &key);
	void start_workers(std::size_t worker_count);
	void reset_closed_state_locked() noexcept;
	void worker_main() noexcept;
	WtPageLoadCompletion load_page(
		const Request &request,
		std::uint64_t &bytes_read
	) const;

	WtAsyncStorageLimits limits_;
	mutable std::mutex mutex_;
	std::condition_variable work_available_;
	std::condition_variable completion_available_;
	std::condition_variable completion_space_available_;
	std::vector<Request> requests_;
	std::vector<RequestIdentity> active_requests_;
	std::vector<WtPageLoadCompletion> completion_slots_;
	std::size_t completion_head_ = 0;
	std::size_t completion_count_ = 0;
	std::uint64_t sequence_counter_ = 0;
	std::vector<InFlightRequest> in_flight_requests_;
	bool open_ = false;
	bool stop_requested_ = false;
	std::vector<std::thread> workers_;
	std::filesystem::path object_root_;
	std::vector<std::uint8_t> manifest_bytes_;
	WtWorldManifestView manifest_;
	bool procedural_ = false;
	WtProceduralWorldDescriptor procedural_descriptor_;
	std::function<void()> completion_notifier_;
	WtAsyncStorageTraceObserver trace_observer_;
	WtAsyncStorageMetrics metrics_;
};

} // namespace world_transvoxel
