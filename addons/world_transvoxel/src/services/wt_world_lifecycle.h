#pragma once

#include "services/wt_runtime_config.h"
#include "storage/wt_async_storage_service.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace world_transvoxel {

enum class WtWorldLifecycleState : std::uint8_t {
	Stopped = 0,
	Starting = 1,
	Running = 2,
	Stopping = 3,
	Failed = 4,
};

enum class WtWorldLifecycleStatus : std::uint8_t {
	Ok,
	InvalidConfiguration,
	InvalidState,
	InvalidPath,
	AlreadyStopped,
	AlreadyStopping,
};

class WtWorldLifecycleService {
public:
	explicit WtWorldLifecycleService(WtRuntimeConfig config);
	~WtWorldLifecycleService();

	WtWorldLifecycleService(const WtWorldLifecycleService &) = delete;
	WtWorldLifecycleService &operator=(const WtWorldLifecycleService &) = delete;

	WtWorldLifecycleStatus start(
		const std::filesystem::path &world_manifest_path,
		const std::filesystem::path &object_root
	);
	WtWorldLifecycleStatus request_stop() noexcept;
	void shutdown_blocking() noexcept;

	WtWorldLifecycleState state() const noexcept;
	WtAsyncStorageStatus last_storage_status() const noexcept;
	std::uint64_t source_revision() const noexcept;
	std::uint64_t world_revision() const noexcept;
	std::size_t page_count() const noexcept;
	bool wait_for_state(
		WtWorldLifecycleState expected,
		std::chrono::milliseconds timeout
	) const;

private:
	void control_main() noexcept;

	WtRuntimeConfig config_;
	bool configuration_valid_ = false;
	mutable std::mutex operation_mutex_;
	mutable std::mutex state_mutex_;
	mutable std::condition_variable state_changed_;
	WtWorldLifecycleState state_ = WtWorldLifecycleState::Stopped;
	WtAsyncStorageStatus last_storage_status_ = WtAsyncStorageStatus::Ok;
	bool stop_requested_ = false;
	std::uint64_t source_revision_ = 0;
	std::uint64_t world_revision_ = 0;
	std::size_t page_count_ = 0;
	std::filesystem::path world_manifest_path_;
	std::filesystem::path object_root_;
	std::unique_ptr<WtAsyncStorageService> storage_;
	std::thread control_thread_;
};

const char *wt_world_lifecycle_state_name(
	WtWorldLifecycleState state
) noexcept;

const char *wt_world_lifecycle_status_message(
	WtWorldLifecycleStatus status
) noexcept;

const char *wt_async_storage_status_message(
	WtAsyncStorageStatus status
) noexcept;

} // namespace world_transvoxel
