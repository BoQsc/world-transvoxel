#include "storage/wt_async_storage_service.h"

#include "storage/wt_procedural_world_source.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace world_transvoxel {
namespace {

bool read_file(
	const std::filesystem::path &path,
	std::size_t maximum_size,
	std::vector<std::uint8_t> &output
) {
	output.clear();
	std::error_code error;
	const std::uintmax_t byte_size = std::filesystem::file_size(path, error);
	if (error || byte_size > maximum_size ||
		byte_size > std::numeric_limits<std::size_t>::max()) {
		return false;
	}
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		return false;
	}
	output.resize(static_cast<std::size_t>(byte_size));
	if (!output.empty()) {
		input.read(
			reinterpret_cast<char *>(output.data()),
			static_cast<std::streamsize>(output.size())
		);
		if (!input || static_cast<std::size_t>(input.gcount()) != output.size()) {
			output.clear();
			return false;
		}
	}
	return true;
}

WtPageLoadStatus page_status(WtWorldPageStatus status) noexcept {
	switch (status) {
		case WtWorldPageStatus::Ok:
			return WtPageLoadStatus::Ok;
		case WtWorldPageStatus::SizeMismatch:
			return WtPageLoadStatus::SizeMismatch;
		case WtWorldPageStatus::HashMismatch:
			return WtPageLoadStatus::HashMismatch;
		case WtWorldPageStatus::MetadataMismatch:
			return WtPageLoadStatus::MetadataMismatch;
		case WtWorldPageStatus::PageNotFound:
		case WtWorldPageStatus::PageFailure:
			return WtPageLoadStatus::PageFailure;
	}
	return WtPageLoadStatus::PageFailure;
}

} // namespace

std::filesystem::path wt_page_object_path(
	const std::filesystem::path &object_root,
	const WtHash256 &content_hash
) {
	constexpr char digits[] = "0123456789abcdef";
	std::string filename;
	filename.resize(content_hash.size() * 2);
	for (std::size_t index = 0; index < content_hash.size(); ++index) {
		filename[index * 2] = digits[content_hash[index] >> 4];
		filename[index * 2 + 1] = digits[content_hash[index] & 0x0fU];
	}
	filename += ".wtchunk";
	return object_root / filename;
}

WtAsyncStorageService::WtAsyncStorageService(WtAsyncStorageLimits limits) :
		limits_(limits) {
}

WtAsyncStorageService::~WtAsyncStorageService() {
	close();
}

bool WtAsyncStorageService::configuration_valid() const noexcept {
	return limits_.request_capacity != 0 &&
		limits_.request_capacity <= kWtMaximumStorageQueueCapacity &&
		limits_.completion_capacity != 0 &&
		limits_.completion_capacity <= kWtMaximumStorageQueueCapacity &&
		limits_.maximum_page_bytes >= kWtContainerHeaderSize &&
		limits_.maximum_page_bytes <= kWtMaximumContainerSize;
}

WtAsyncStorageStatus WtAsyncStorageService::open(
	const std::filesystem::path &world_manifest_path,
	const std::filesystem::path &object_root
) {
	if (!configuration_valid()) {
		return WtAsyncStorageStatus::InvalidConfiguration;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (open_) {
			return WtAsyncStorageStatus::AlreadyOpen;
		}
	}
	std::error_code error;
	if (!std::filesystem::is_regular_file(world_manifest_path, error) || error) {
		return WtAsyncStorageStatus::InvalidPath;
	}
	error.clear();
	if (!std::filesystem::is_directory(object_root, error) || error) {
		return WtAsyncStorageStatus::InvalidPath;
	}
	std::vector<std::uint8_t> manifest_bytes;
	if (!read_file(
			world_manifest_path,
			kWtMaximumContainerSize,
			manifest_bytes
		)) {
		return WtAsyncStorageStatus::ManifestIoFailure;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	requests_.reserve(limits_.request_capacity);
	active_requests_.reserve(
		limits_.request_capacity + limits_.completion_capacity + 1
	);
	completion_slots_.assign(limits_.completion_capacity, {});
	manifest_bytes_ = std::move(manifest_bytes);
	if (wt_open_world_manifest(
			{ manifest_bytes_.data(), manifest_bytes_.size() },
			manifest_
		) != WtWorldManifestStatus::Ok) {
		manifest_bytes_.clear();
		manifest_ = {};
		return WtAsyncStorageStatus::ManifestFailure;
	}
	for (const WtWorldPageIndexEntry &entry : manifest_.pages) {
		if (entry.byte_size > limits_.maximum_page_bytes) {
			manifest_bytes_.clear();
			manifest_ = {};
			return WtAsyncStorageStatus::PageSizeLimitExceeded;
		}
	}
	object_root_ = object_root;
	procedural_ = false;
	procedural_descriptor_ = {};
	procedural_keys_.clear();
	requests_.clear();
	active_requests_.clear();
	completion_head_ = 0;
	completion_count_ = 0;
	sequence_counter_ = 0;
	stop_requested_ = false;
	metrics_ = {};
	open_ = true;
	worker_ = std::thread(&WtAsyncStorageService::worker_main, this);
	return WtAsyncStorageStatus::Ok;
}

WtAsyncStorageStatus WtAsyncStorageService::open_procedural(
	const WtProceduralWorldDescriptor &descriptor
) {
	if (!configuration_valid()) {
		return WtAsyncStorageStatus::InvalidConfiguration;
	}
	if (!wt_valid_procedural_descriptor(descriptor)) {
		return WtAsyncStorageStatus::InvalidConfiguration;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (open_) {
			return WtAsyncStorageStatus::AlreadyOpen;
		}
	}
	std::lock_guard<std::mutex> lock(mutex_);
	requests_.reserve(limits_.request_capacity);
	active_requests_.reserve(
		limits_.request_capacity + limits_.completion_capacity + 1
	);
	completion_slots_.assign(limits_.completion_capacity, {});
	manifest_bytes_.clear();
	manifest_ = {};
	object_root_.clear();
	procedural_ = true;
	procedural_descriptor_ = descriptor;
	procedural_keys_ = wt_procedural_keys(descriptor);
	requests_.clear();
	active_requests_.clear();
	completion_head_ = 0;
	completion_count_ = 0;
	sequence_counter_ = 0;
	stop_requested_ = false;
	metrics_ = {};
	open_ = true;
	worker_ = std::thread(&WtAsyncStorageService::worker_main, this);
	return WtAsyncStorageStatus::Ok;
}

void WtAsyncStorageService::close() noexcept {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!open_) {
			return;
		}
		stop_requested_ = true;
		metrics_.cancelled_requests += requests_.size();
		metrics_.discarded_completions += completion_count_;
		requests_.clear();
		work_available_.notify_all();
		completion_available_.notify_all();
		completion_space_available_.notify_all();
	}
	if (worker_.joinable()) {
		worker_.join();
	}
	std::lock_guard<std::mutex> lock(mutex_);
	open_ = false;
	stop_requested_ = false;
	active_requests_.clear();
	completion_head_ = 0;
	completion_count_ = 0;
	object_root_.clear();
	manifest_bytes_.clear();
	manifest_ = {};
	procedural_ = false;
	procedural_descriptor_ = {};
	procedural_keys_.clear();
	completion_notifier_ = {};
}

WtAsyncStorageStatus WtAsyncStorageService::request_page(
	const WtChunkKey &key,
	WtGenerationToken generation,
	std::int32_t priority
) {
	if (!wt_is_valid_chunk_key(key) || generation.value == 0) {
		return WtAsyncStorageStatus::InvalidKey;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	if (!open_ || stop_requested_) {
		return WtAsyncStorageStatus::NotOpen;
	}
	const WtWorldPageIndexEntry *entry = procedural_ ?
		nullptr : manifest_.find_page(key);
	if ((procedural_ &&
			!wt_procedural_can_generate_page(procedural_descriptor_, key)) ||
		(!procedural_ && entry == nullptr)) {
		return WtAsyncStorageStatus::PageNotFound;
	}
	const auto active = std::find_if(
		active_requests_.begin(),
		active_requests_.end(),
		[&](const RequestIdentity &identity) {
			return identity.key == key && identity.generation == generation;
		}
	);
	if (active != active_requests_.end()) {
		++metrics_.duplicate_requests;
		return WtAsyncStorageStatus::AlreadyPending;
	}
	if (requests_.size() >= limits_.request_capacity) {
		++metrics_.request_queue_rejections;
		return WtAsyncStorageStatus::RequestQueueFull;
	}
	Request request;
	request.key = key;
	request.generation = generation;
	if (entry != nullptr) {
		request.entry = *entry;
	}
	request.sequence = ++sequence_counter_;
	request.priority = priority;
	const auto position = std::lower_bound(
		requests_.begin(),
		requests_.end(),
		request,
		[](const Request &left, const Request &right) {
			if (left.priority != right.priority) {
				return left.priority > right.priority;
			}
			return left.sequence < right.sequence;
		}
	);
	requests_.insert(position, request);
	active_requests_.push_back({ key, generation });
	++metrics_.accepted_requests;
	work_available_.notify_one();
	return WtAsyncStorageStatus::Ok;
}

WtPageLoadStatus WtAsyncStorageService::load_page_now(
	const WtChunkKey &key,
	std::shared_ptr<const std::vector<std::uint8_t>> &page_bytes
) const {
	page_bytes.reset();
	Request request;
	WtProceduralWorldDescriptor procedural_descriptor;
	bool procedural = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!open_ || stop_requested_) return WtPageLoadStatus::IoFailure;
		procedural = procedural_;
		procedural_descriptor = procedural_descriptor_;
		const WtWorldPageIndexEntry *entry = procedural ?
			nullptr : manifest_.find_page(key);
		if ((procedural &&
				!wt_procedural_can_generate_page(procedural_descriptor, key)) ||
			(!procedural && entry == nullptr)) {
			return WtPageLoadStatus::PageFailure;
		}
		request.key = key;
		if (entry != nullptr) {
			request.entry = *entry;
		}
	}
	std::uint64_t bytes_read = 0;
	WtPageLoadCompletion completion = procedural ?
		wt_generate_procedural_page(
			procedural_descriptor, key, {}, bytes_read
		) :
		load_page(request, bytes_read);
	if (completion.status == WtPageLoadStatus::Ok) {
		page_bytes = std::move(completion.page_bytes);
	}
	return completion.status;
}

bool WtAsyncStorageService::snapshot_manifest(
	std::vector<std::uint8_t> &manifest_bytes
) const {
	std::lock_guard<std::mutex> lock(mutex_);
	manifest_bytes.clear();
	if (!open_ || stop_requested_ || procedural_) return false;
	manifest_bytes = manifest_bytes_;
	return true;
}

bool WtAsyncStorageService::sample_procedural_base(
	const WtGridPoint &point,
	WtScalarSample &output
) const noexcept {
	WtProceduralWorldDescriptor descriptor;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!open_ || stop_requested_ || !procedural_) {
			return false;
		}
		descriptor = procedural_descriptor_;
	}
	return wt_sample_procedural_world(descriptor, point, output);
}

bool WtAsyncStorageService::pop_completion_locked(
	WtPageLoadCompletion &completion
) {
	if (completion_count_ == 0) {
		return false;
	}
	completion = std::move(completion_slots_[completion_head_]);
	completion_slots_[completion_head_] = {};
	completion_head_ =
		(completion_head_ + 1) % limits_.completion_capacity;
	--completion_count_;
	remove_active_locked(completion.key, completion.generation);
	completion_space_available_.notify_one();
	return true;
}

bool WtAsyncStorageService::pop_completion(
	WtPageLoadCompletion &completion
) {
	std::lock_guard<std::mutex> lock(mutex_);
	return pop_completion_locked(completion);
}

bool WtAsyncStorageService::wait_pop_completion(
	WtPageLoadCompletion &completion,
	std::chrono::milliseconds timeout
) {
	std::unique_lock<std::mutex> lock(mutex_);
	if (!completion_available_.wait_for(
			lock,
			timeout,
			[&]() {
				return completion_count_ != 0 || !open_ || stop_requested_;
			}
		)) {
		return false;
	}
	return pop_completion_locked(completion);
}

void WtAsyncStorageService::remove_active_locked(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	const auto active = std::find_if(
		active_requests_.begin(),
		active_requests_.end(),
		[&](const RequestIdentity &identity) {
			return identity.key == key && identity.generation == generation;
		}
	);
	if (active != active_requests_.end()) {
		active_requests_.erase(active);
	}
}

void WtAsyncStorageService::worker_main() noexcept {
	for (;;) {
		Request request;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			work_available_.wait(lock, [&]() {
				return stop_requested_ || !requests_.empty();
			});
			if (stop_requested_) {
				return;
			}
			request = requests_.front();
			requests_.erase(requests_.begin());
			++metrics_.started_requests;
		}

		std::uint64_t bytes_read = 0;
		WtPageLoadCompletion completion = load_page(request, bytes_read);

		std::unique_lock<std::mutex> lock(mutex_);
		completion_space_available_.wait(lock, [&]() {
			return stop_requested_ ||
				completion_count_ < limits_.completion_capacity;
		});
		if (stop_requested_) {
			++metrics_.cancelled_requests;
			return;
		}
		const std::size_t tail =
			(completion_head_ + completion_count_) %
			limits_.completion_capacity;
		completion_slots_[tail] = std::move(completion);
		++completion_count_;
		++metrics_.completed_requests;
		metrics_.bytes_read += bytes_read;
		if (completion_slots_[tail].status == WtPageLoadStatus::Ok) {
			++metrics_.successful_pages;
		} else {
			++metrics_.failed_pages;
		}
		completion_available_.notify_one();
		if (completion_notifier_) completion_notifier_();
	}
}

WtPageLoadCompletion WtAsyncStorageService::load_page(
	const Request &request,
	std::uint64_t &bytes_read
) const {
	WtPageLoadCompletion completion;
	completion.key = request.key;
	completion.generation = request.generation;
	if (procedural_) {
		return wt_generate_procedural_page(
			procedural_descriptor_,
			request.key,
			request.generation,
			bytes_read
		);
	}
	const std::filesystem::path path =
		wt_page_object_path(object_root_, request.entry.content_hash);
	std::error_code error;
	const bool exists = std::filesystem::exists(path, error);
	if (error) {
		completion.status = WtPageLoadStatus::IoFailure;
		return completion;
	}
	if (!exists) {
		completion.status = WtPageLoadStatus::ObjectMissing;
		return completion;
	}
	const std::uintmax_t stored_size = std::filesystem::file_size(path, error);
	if (error) {
		completion.status = WtPageLoadStatus::IoFailure;
		return completion;
	}
	if (stored_size != request.entry.byte_size) {
		completion.status = WtPageLoadStatus::SizeMismatch;
		return completion;
	}
	auto bytes = std::make_shared<std::vector<std::uint8_t>>();
	if (!read_file(path, limits_.maximum_page_bytes, *bytes)) {
		completion.status = WtPageLoadStatus::IoFailure;
		return completion;
	}
	bytes_read = bytes->size();
	completion.status = page_status(wt_validate_world_page(
		manifest_,
		request.key,
		{ bytes->data(), bytes->size() }
	));
	if (completion.status == WtPageLoadStatus::Ok) {
		completion.page_bytes = std::move(bytes);
	}
	return completion;
}

bool WtAsyncStorageService::is_open() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	return open_ && !stop_requested_;
}

void WtAsyncStorageService::set_completion_notifier(
	std::function<void()> notifier
) {
	std::lock_guard<std::mutex> lock(mutex_);
	completion_notifier_ = std::move(notifier);
}

bool WtAsyncStorageService::has_page(const WtChunkKey &key) const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	return open_ && (procedural_ ?
		wt_procedural_has_key(procedural_keys_, key) :
		manifest_.find_page(key) != nullptr);
}

std::vector<WtChunkKey> WtAsyncStorageService::page_keys() const {
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<WtChunkKey> keys;
	if (!open_) return keys;
	if (procedural_) return procedural_keys_;
	keys.reserve(manifest_.pages.size());
	for (const WtWorldPageIndexEntry &entry : manifest_.pages) {
		keys.push_back(entry.key);
	}
	return keys;
}

std::uint64_t WtAsyncStorageService::source_revision() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!open_) return 0;
	return procedural_ ? procedural_descriptor_.source_revision :
		manifest_.source_revision;
}

std::uint64_t WtAsyncStorageService::world_revision() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!open_) return 0;
	return procedural_ ? procedural_descriptor_.world_revision :
		manifest_.world_revision;
}

std::size_t WtAsyncStorageService::page_count() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!open_) return 0;
	return procedural_ ? procedural_keys_.size() : manifest_.pages.size();
}

std::size_t WtAsyncStorageService::queued_request_count() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	return requests_.size();
}

std::size_t WtAsyncStorageService::queued_completion_count() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	return completion_count_;
}

WtAsyncStorageMetrics WtAsyncStorageService::get_metrics() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	return metrics_;
}

} // namespace world_transvoxel
