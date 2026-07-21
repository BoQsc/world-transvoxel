#include "services/wt_world_lifecycle.h"

#include <system_error>
#include <utility>

namespace world_transvoxel {

WtWorldLifecycleService::WtWorldLifecycleService(WtRuntimeConfig config) :
		config_(config),
		configuration_valid_(
			wt_validate_runtime_config(config) == WtRuntimeConfigStatus::Ok
		) {
}

WtWorldLifecycleService::~WtWorldLifecycleService() {
	shutdown_blocking();
}

WtWorldLifecycleStatus WtWorldLifecycleService::start(
	const std::filesystem::path &world_manifest_path,
	const std::filesystem::path &object_root
) {
	std::lock_guard<std::mutex> operation_lock(operation_mutex_);
	if (!configuration_valid_) {
		return WtWorldLifecycleStatus::InvalidConfiguration;
	}
	if (world_manifest_path.empty() || object_root.empty()) {
		return WtWorldLifecycleStatus::InvalidPath;
	}
	{
		std::lock_guard<std::mutex> state_lock(state_mutex_);
		if (state_ != WtWorldLifecycleState::Stopped) {
			return WtWorldLifecycleStatus::InvalidState;
		}
	}
	if (control_thread_.joinable()) {
		control_thread_.join();
	}
	{
		std::lock_guard<std::mutex> state_lock(state_mutex_);
		world_manifest_path_ = world_manifest_path;
		object_root_ = object_root;
		procedural_ = false;
		procedural_descriptor_ = {};
		storage_ = std::make_unique<WtAsyncStorageService>(
			WtAsyncStorageLimits {
				static_cast<std::size_t>(config_.storage_request_capacity),
				static_cast<std::size_t>(config_.storage_completion_capacity),
				kWtMaximumContainerSize,
			}
		);
		last_storage_status_ = WtAsyncStorageStatus::Ok;
		last_runtime_status_ = WtReadOnlyRuntimeStatus::Ok;
		last_edit_journal_status_ = WtEditJournalStoreStatus::Ok;
		stop_requested_ = false;
		source_revision_ = 0;
		world_revision_ = 0;
		page_count_ = 0;
		state_ = WtWorldLifecycleState::Starting;
	}
	state_changed_.notify_all();
	control_thread_ = std::thread(
		&WtWorldLifecycleService::control_main,
		this
	);
	return WtWorldLifecycleStatus::Ok;
}

WtWorldLifecycleStatus WtWorldLifecycleService::start_procedural(
	const WtProceduralWorldDescriptor &descriptor,
	const std::filesystem::path &object_root
) {
	std::lock_guard<std::mutex> operation_lock(operation_mutex_);
	if (!configuration_valid_) {
		return WtWorldLifecycleStatus::InvalidConfiguration;
	}
	if (object_root.empty()) {
		return WtWorldLifecycleStatus::InvalidPath;
	}
	{
		std::lock_guard<std::mutex> state_lock(state_mutex_);
		if (state_ != WtWorldLifecycleState::Stopped) {
			return WtWorldLifecycleStatus::InvalidState;
		}
	}
	if (control_thread_.joinable()) {
		control_thread_.join();
	}
	{
		std::lock_guard<std::mutex> state_lock(state_mutex_);
		world_manifest_path_.clear();
		object_root_ = object_root;
		procedural_ = true;
		procedural_descriptor_ = descriptor;
		storage_ = std::make_unique<WtAsyncStorageService>(
			WtAsyncStorageLimits {
				static_cast<std::size_t>(config_.storage_request_capacity),
				static_cast<std::size_t>(config_.storage_completion_capacity),
				kWtMaximumContainerSize,
			}
		);
		last_storage_status_ = WtAsyncStorageStatus::Ok;
		last_runtime_status_ = WtReadOnlyRuntimeStatus::Ok;
		last_edit_journal_status_ = WtEditJournalStoreStatus::Ok;
		stop_requested_ = false;
		source_revision_ = 0;
		world_revision_ = 0;
		page_count_ = 0;
		state_ = WtWorldLifecycleState::Starting;
	}
	state_changed_.notify_all();
	control_thread_ = std::thread(
		&WtWorldLifecycleService::control_main,
		this
	);
	return WtWorldLifecycleStatus::Ok;
}

WtWorldLifecycleStatus WtWorldLifecycleService::request_stop() noexcept {
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (state_ == WtWorldLifecycleState::Stopped) {
		return WtWorldLifecycleStatus::AlreadyStopped;
	}
	if (state_ == WtWorldLifecycleState::Stopping) {
		return WtWorldLifecycleStatus::AlreadyStopping;
	}
	stop_requested_ = true;
	state_ = WtWorldLifecycleState::Stopping;
	if (runtime_) runtime_->request_stop();
	state_changed_.notify_all();
	return WtWorldLifecycleStatus::Ok;
}

void WtWorldLifecycleService::shutdown_blocking() noexcept {
	std::lock_guard<std::mutex> operation_lock(operation_mutex_);
	request_stop();
	if (control_thread_.joinable()) {
		control_thread_.join();
	}
}

void WtWorldLifecycleService::control_main() noexcept {
	std::error_code directory_error;
	if (procedural_) {
		std::filesystem::create_directories(object_root_, directory_error);
	}
	const WtAsyncStorageStatus open_status =
		directory_error ? WtAsyncStorageStatus::InvalidPath :
		procedural_ ? storage_->open_procedural(procedural_descriptor_) :
		storage_->open(world_manifest_path_, object_root_);
	WtEditJournalStoreStatus journal_status =
		WtEditJournalStoreStatus::NotOpen;
	if (open_status == WtAsyncStorageStatus::Ok) {
		edit_journal_store_ = std::make_unique<WtEditJournalStore>();
		journal_status = edit_journal_store_->open(
			object_root_ / "world.wtedit",
			storage_->source_revision(),
			storage_->world_revision()
		);
	}
	bool run_runtime = false;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		last_storage_status_ = open_status;
		last_edit_journal_status_ = open_status == WtAsyncStorageStatus::Ok ?
			journal_status : WtEditJournalStoreStatus::Ok;
		if (stop_requested_) {
			state_ = WtWorldLifecycleState::Stopping;
		} else if (open_status == WtAsyncStorageStatus::Ok &&
			journal_status == WtEditJournalStoreStatus::Ok) {
			source_revision_ = storage_->source_revision();
			world_revision_ = edit_journal_store_->current_world_revision();
			page_count_ = storage_->page_count();
			runtime_ = std::make_unique<WtReadOnlyWorldRuntime>(
				config_,
				*storage_,
				edit_journal_store_.get()
			);
			if (runtime_->valid()) {
				state_ = WtWorldLifecycleState::Running;
				run_runtime = true;
			} else {
				last_runtime_status_ =
					WtReadOnlyRuntimeStatus::InvalidConfiguration;
				state_ = WtWorldLifecycleState::Failed;
			}
		} else {
			state_ = WtWorldLifecycleState::Failed;
		}
		state_changed_.notify_all();
	}
	if (run_runtime) {
		const WtReadOnlyRuntimeStatus runtime_status = runtime_->run();
		std::lock_guard<std::mutex> lock(state_mutex_);
		last_runtime_status_ = runtime_status;
		state_ = stop_requested_ ? WtWorldLifecycleState::Stopping :
			WtWorldLifecycleState::Failed;
		state_changed_.notify_all();
	}
	{
		std::unique_lock<std::mutex> lock(state_mutex_);
		if (!stop_requested_) {
			state_changed_.wait(lock, [&]() { return stop_requested_; });
		}
		state_ = WtWorldLifecycleState::Stopping;
		if (runtime_) runtime_->request_stop();
		state_changed_.notify_all();
	}
	storage_->close();
	if (edit_journal_store_) edit_journal_store_->close();
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		runtime_.reset();
		storage_.reset();
		edit_journal_store_.reset();
		world_manifest_path_.clear();
		object_root_.clear();
		procedural_ = false;
		procedural_descriptor_ = {};
		stop_requested_ = false;
		source_revision_ = 0;
		world_revision_ = 0;
		page_count_ = 0;
		state_ = WtWorldLifecycleState::Stopped;
	}
	state_changed_.notify_all();
}

WtWorldLifecycleState WtWorldLifecycleService::state() const noexcept {
	std::lock_guard<std::mutex> lock(state_mutex_);
	return state_;
}

WtAsyncStorageStatus
WtWorldLifecycleService::last_storage_status() const noexcept {
	std::lock_guard<std::mutex> lock(state_mutex_);
	return last_storage_status_;
}

WtReadOnlyRuntimeStatus
WtWorldLifecycleService::last_runtime_status() const noexcept {
	std::lock_guard<std::mutex> lock(state_mutex_);
	return last_runtime_status_;
}

WtReadOnlyRuntimeMetrics
WtWorldLifecycleService::runtime_metrics() const noexcept {
	std::lock_guard<std::mutex> lock(state_mutex_);
	return runtime_ ? runtime_->get_metrics() : WtReadOnlyRuntimeMetrics{};
}

WtEditJournalStoreStatus
WtWorldLifecycleService::last_edit_journal_status() const noexcept {
	std::lock_guard<std::mutex> lock(state_mutex_);
	return last_edit_journal_status_;
}

WtReadOnlyRuntimeStatus WtWorldLifecycleService::update_viewer(
	const WtViewerSnapshot &snapshot,
	std::uint32_t radius_chunks,
	std::uint8_t maximum_lod
) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (state_ != WtWorldLifecycleState::Running || !runtime_) {
		return WtReadOnlyRuntimeStatus::NotRunning;
	}
	return runtime_->update_viewer(snapshot, radius_chunks, maximum_lod);
}

WtReadOnlyRuntimeStatus WtWorldLifecycleService::remove_viewer(
	std::uint64_t viewer_id,
	std::uint64_t revision
) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (state_ != WtWorldLifecycleState::Running || !runtime_) {
		return WtReadOnlyRuntimeStatus::NotRunning;
	}
	return runtime_->remove_viewer(viewer_id, revision);
}

WtReadOnlyRuntimeStatus WtWorldLifecycleService::update_collision_viewer(
	const WtViewerSnapshot &snapshot,
	std::uint32_t radius_chunks
) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (state_ != WtWorldLifecycleState::Running || !runtime_) {
		return WtReadOnlyRuntimeStatus::NotRunning;
	}
	return runtime_->update_collision_viewer(snapshot, radius_chunks);
}

WtReadOnlyRuntimeStatus WtWorldLifecycleService::remove_collision_viewer(
	std::uint64_t viewer_id,
	std::uint64_t revision
) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (state_ != WtWorldLifecycleState::Running || !runtime_) {
		return WtReadOnlyRuntimeStatus::NotRunning;
	}
	return runtime_->remove_collision_viewer(viewer_id, revision);
}

WtReadOnlyRuntimeStatus WtWorldLifecycleService::submit_edit(
	const WtEditTransaction &transaction
) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (state_ != WtWorldLifecycleState::Running || !runtime_) {
		return WtReadOnlyRuntimeStatus::NotRunning;
	}
	return runtime_->submit_edit(transaction);
}

WtReadOnlyRuntimeStatus
WtWorldLifecycleService::request_authoritative_sample(
	const WtGridPoint &point,
	std::uint8_t lod,
	std::uint64_t &request_id
) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (state_ != WtWorldLifecycleState::Running || !runtime_) {
		request_id = 0;
		return WtReadOnlyRuntimeStatus::NotRunning;
	}
	return runtime_->request_authoritative_sample(point, lod, request_id);
}

WtReadOnlyRuntimeStatus
WtWorldLifecycleService::request_authoritative_samples(
	const std::vector<WtGridPoint> &points,
	std::uint8_t lod,
	std::uint64_t &request_id
) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (state_ != WtWorldLifecycleState::Running || !runtime_) {
		request_id = 0;
		return WtReadOnlyRuntimeStatus::NotRunning;
	}
	return runtime_->request_authoritative_samples(points, lod, request_id);
}

WtReadOnlyRuntimeStatus WtWorldLifecycleService::request_world_snapshot(
	const std::filesystem::path &output_directory,
	std::uint64_t new_source_revision,
	bool compact,
	std::uint64_t &request_id
) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (state_ != WtWorldLifecycleState::Running || !runtime_) {
		request_id = 0;
		return WtReadOnlyRuntimeStatus::NotRunning;
	}
	return runtime_->request_world_snapshot(
		output_directory,
		new_source_revision,
		compact,
		request_id
	);
}

bool WtWorldLifecycleService::pop_publication(
	WtReadOnlyPublication &publication
) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	return runtime_ && runtime_->pop_publication(publication);
}

std::uint64_t WtWorldLifecycleService::source_revision() const noexcept {
	std::lock_guard<std::mutex> lock(state_mutex_);
	return source_revision_;
}

std::uint64_t WtWorldLifecycleService::world_revision() const noexcept {
	std::lock_guard<std::mutex> lock(state_mutex_);
	return runtime_ ? runtime_->world_revision() : world_revision_;
}

std::size_t WtWorldLifecycleService::page_count() const noexcept {
	std::lock_guard<std::mutex> lock(state_mutex_);
	return page_count_;
}

bool WtWorldLifecycleService::wait_for_state(
	WtWorldLifecycleState expected,
	std::chrono::milliseconds timeout
) const {
	std::unique_lock<std::mutex> lock(state_mutex_);
	return state_changed_.wait_for(
		lock,
		timeout,
		[&]() { return state_ == expected; }
	);
}

const char *wt_world_lifecycle_state_name(
	WtWorldLifecycleState state
) noexcept {
	switch (state) {
		case WtWorldLifecycleState::Stopped: return "stopped";
		case WtWorldLifecycleState::Starting: return "starting";
		case WtWorldLifecycleState::Running: return "running";
		case WtWorldLifecycleState::Stopping: return "stopping";
		case WtWorldLifecycleState::Failed: return "failed";
	}
	return "unknown";
}

const char *wt_world_lifecycle_status_message(
	WtWorldLifecycleStatus status
) noexcept {
	switch (status) {
		case WtWorldLifecycleStatus::Ok: return "ok";
		case WtWorldLifecycleStatus::InvalidConfiguration:
			return "runtime configuration is invalid";
		case WtWorldLifecycleStatus::InvalidState:
			return "world lifecycle state does not allow startup";
		case WtWorldLifecycleStatus::InvalidPath:
			return "manifest and object-root paths are required";
		case WtWorldLifecycleStatus::AlreadyStopped:
			return "world is already stopped";
		case WtWorldLifecycleStatus::AlreadyStopping:
			return "world is already stopping";
	}
	return "unknown lifecycle status";
}

const char *wt_async_storage_status_message(
	WtAsyncStorageStatus status
) noexcept {
	switch (status) {
		case WtAsyncStorageStatus::Ok: return "ok";
		case WtAsyncStorageStatus::InvalidConfiguration:
			return "storage configuration is invalid";
		case WtAsyncStorageStatus::AlreadyOpen:
			return "storage is already open";
		case WtAsyncStorageStatus::NotOpen: return "storage is not open";
		case WtAsyncStorageStatus::InvalidPath:
			return "manifest or object-root path is invalid";
		case WtAsyncStorageStatus::ManifestIoFailure:
			return "manifest could not be read";
		case WtAsyncStorageStatus::ManifestFailure:
			return "manifest validation failed";
		case WtAsyncStorageStatus::PageSizeLimitExceeded:
			return "manifest page exceeds the configured size limit";
		case WtAsyncStorageStatus::InvalidKey:
			return "page request identity is invalid";
		case WtAsyncStorageStatus::PageNotFound:
			return "page is not indexed by the manifest";
		case WtAsyncStorageStatus::AlreadyPending:
			return "page request is already pending";
		case WtAsyncStorageStatus::RequestQueueFull:
			return "storage request queue is full";
	}
	return "unknown storage status";
}

} // namespace world_transvoxel
