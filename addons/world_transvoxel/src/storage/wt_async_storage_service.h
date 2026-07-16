#pragma once

#include "core/wt_chunk_state.h"
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

struct WtAsyncStorageLimits {
	std::size_t request_capacity = 256;
	std::size_t completion_capacity = 256;
	std::size_t maximum_page_bytes = kWtMaximumContainerSize;
};

enum class WtProceduralWorldMode : std::uint8_t {
	Terrain = 0,
	Flat = 1,
	RollingHillsCave = 2,
};

struct WtProceduralWorldDescriptor {
	std::uint32_t chunk_count_x = 0;
	std::uint32_t chunk_count_y = 8;
	std::uint32_t chunk_count_z = 0;
	std::int32_t chunk_y = 0;
	std::uint64_t source_revision = 0;
	std::uint64_t world_revision = 0;
	std::uint32_t seed = 1;
	WtProceduralWorldMode mode = WtProceduralWorldMode::Terrain;
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
};

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
	bool pop_completion(WtPageLoadCompletion &completion);
	bool wait_pop_completion(
		WtPageLoadCompletion &completion,
		std::chrono::milliseconds timeout
	);
	void set_completion_notifier(std::function<void()> notifier);

	bool is_open() const noexcept;
	bool has_page(const WtChunkKey &key) const noexcept;
	std::vector<WtChunkKey> page_keys() const;
	std::uint64_t source_revision() const noexcept;
	std::uint64_t world_revision() const noexcept;
	std::size_t page_count() const noexcept;
	std::size_t queued_request_count() const noexcept;
	std::size_t queued_completion_count() const noexcept;
	WtAsyncStorageMetrics get_metrics() const noexcept;

private:
	struct Request {
		WtChunkKey key;
		WtGenerationToken generation;
		WtWorldPageIndexEntry entry;
		std::uint64_t sequence = 0;
		std::int32_t priority = 0;
	};

	struct RequestIdentity {
		WtChunkKey key;
		WtGenerationToken generation;
	};

	bool configuration_valid() const noexcept;
	bool pop_completion_locked(WtPageLoadCompletion &completion);
	void remove_active_locked(
		const WtChunkKey &key,
		WtGenerationToken generation
	);
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
	bool open_ = false;
	bool stop_requested_ = false;
	std::thread worker_;
	std::filesystem::path object_root_;
	std::vector<std::uint8_t> manifest_bytes_;
	WtWorldManifestView manifest_;
	bool procedural_ = false;
	WtProceduralWorldDescriptor procedural_descriptor_;
	std::vector<WtChunkKey> procedural_keys_;
	std::function<void()> completion_notifier_;
	WtAsyncStorageMetrics metrics_;
};

} // namespace world_transvoxel
