#include "storage/wt_edit_journal_store.h"

#include <cstdio>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace world_transvoxel {
namespace {

FILE *open_append(const std::filesystem::path &path) {
#if defined(_WIN32)
	return _wfopen(path.c_str(), L"ab");
#else
	return std::fopen(path.c_str(), "ab");
#endif
}

FILE *open_update(const std::filesystem::path &path) {
#if defined(_WIN32)
	return _wfopen(path.c_str(), L"rb+");
#else
	return std::fopen(path.c_str(), "rb+");
#endif
}

bool flush_durable(FILE *file) noexcept {
	if (file == nullptr || std::fflush(file) != 0) return false;
#if defined(_WIN32)
	return _commit(_fileno(file)) == 0;
#else
	return fsync(fileno(file)) == 0;
#endif
}

bool sync_file(const std::filesystem::path &path) noexcept {
	FILE *file = open_update(path);
	if (file == nullptr) return false;
	const bool ok = flush_durable(file);
	return std::fclose(file) == 0 && ok;
}

bool resize_and_sync(
	const std::filesystem::path &path,
	std::uintmax_t size
) noexcept {
	std::error_code error;
	std::filesystem::resize_file(path, size, error);
	return !error && sync_file(path);
}

WtEditJournalStoreStatus map_journal_status(
	WtEditJournalStatus status
) noexcept {
	switch (status) {
		case WtEditJournalStatus::Ok:
		case WtEditJournalStatus::RecoveredTruncatedTail:
			return WtEditJournalStoreStatus::Ok;
		case WtEditJournalStatus::TransactionCapacityExceeded:
		case WtEditJournalStatus::CommandCapacityExceeded:
		case WtEditJournalStatus::ByteCapacityExceeded:
			return WtEditJournalStoreStatus::CapacityExceeded;
		case WtEditJournalStatus::CorruptJournal:
			return WtEditJournalStoreStatus::CorruptJournal;
		default:
			return WtEditJournalStoreStatus::JournalFailure;
	}
}

} // namespace

struct WtEditJournalStore::AsyncState {
	struct Job {
		std::vector<std::uint8_t> bytes;
		std::uintmax_t previous_size = 0;
	};

	explicit AsyncState(std::filesystem::path value, std::uintmax_t size) :
			path(std::move(value)), scheduled_size(size),
			worker([this]() { run(); }) {
	}

	~AsyncState() {
		flush();
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
		}
		available.notify_all();
		if (worker.joinable()) worker.join();
	}

	WtEditJournalStoreStatus commit_and_enqueue(
		WtEditJournal &journal,
		std::vector<std::uint8_t> segment
	) {
		std::lock_guard<std::mutex> lock(mutex);
		if (failure != WtEditJournalStoreStatus::Ok || stopping ||
			pending_bytes > kWtProductionJournalByteCapacity - segment.size()) {
			return failure == WtEditJournalStoreStatus::Ok ?
				WtEditJournalStoreStatus::CapacityExceeded : failure;
		}
		const WtEditJournalStatus committed = journal.commit_append({
			segment.data(), segment.size(),
		});
		if (committed != WtEditJournalStatus::Ok) {
			return map_journal_status(committed);
		}
		const std::size_t size = segment.size();
		jobs.push_back({ std::move(segment), scheduled_size });
		scheduled_size += size;
		pending_bytes += size;
		available.notify_one();
		return WtEditJournalStoreStatus::Ok;
	}

	WtEditJournalStoreStatus flush() {
		std::unique_lock<std::mutex> lock(mutex);
		completed.wait(lock, [this]() {
			return failure != WtEditJournalStoreStatus::Ok ||
				(jobs.empty() && !active);
		});
		return failure;
	}

	WtEditJournalStoreStatus status() const noexcept {
		std::lock_guard<std::mutex> lock(mutex);
		return failure;
	}

	void run() {
		while (true) {
			Job job;
			{
				std::unique_lock<std::mutex> lock(mutex);
				available.wait(lock, [this]() { return stopping || !jobs.empty(); });
				if (stopping && jobs.empty()) return;
				job = std::move(jobs.front());
				jobs.pop_front();
				active = true;
			}
			std::error_code error;
			const bool exists = std::filesystem::exists(path, error);
			const std::uintmax_t actual_size = !error && exists ?
				std::filesystem::file_size(path, error) : 0;
			FILE *file = !error && actual_size == job.previous_size ?
				open_append(path) : nullptr;
			const bool written = file != nullptr && std::fwrite(
				job.bytes.data(), 1, job.bytes.size(), file
			) == job.bytes.size();
			const bool durable = written && flush_durable(file);
			const bool closed = file != nullptr && std::fclose(file) == 0;
			if (!(durable && closed)) {
				resize_and_sync(path, job.previous_size);
			}
			{
				std::lock_guard<std::mutex> lock(mutex);
				pending_bytes -= job.bytes.size();
				active = false;
				if (!(durable && closed)) {
					failure = WtEditJournalStoreStatus::IoFailure;
					jobs.clear();
					pending_bytes = 0;
				}
			}
			completed.notify_all();
		}
	}

	std::filesystem::path path;
	mutable std::mutex mutex;
	std::condition_variable available;
	std::condition_variable completed;
	std::deque<Job> jobs;
	std::uintmax_t scheduled_size = 0;
	std::size_t pending_bytes = 0;
	bool active = false;
	bool stopping = false;
	WtEditJournalStoreStatus failure = WtEditJournalStoreStatus::Ok;
	std::thread worker;
};

WtEditJournalStore::WtEditJournalStore() :
		journal_(
			kWtProductionJournalTransactionCapacity,
			kWtProductionJournalCommandCapacity,
			kWtProductionJournalByteCapacity
		) {
}

WtEditJournalStore::~WtEditJournalStore() {
	close();
}

WtEditJournalStoreStatus WtEditJournalStore::open(
	const std::filesystem::path &path,
	std::uint64_t source_revision,
	std::uint64_t initial_world_revision
) {
	close();
	if (path.empty()) return WtEditJournalStoreStatus::InvalidPath;
	std::error_code error;
	const bool exists = std::filesystem::exists(path, error);
	if (error) return WtEditJournalStoreStatus::IoFailure;
	if (!exists) {
		journal_.reset(source_revision, initial_world_revision);
		path_ = path;
		open_ = true;
		return WtEditJournalStoreStatus::Ok;
	}
	if (!std::filesystem::is_regular_file(path, error) || error) {
		return WtEditJournalStoreStatus::InvalidPath;
	}
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error) return WtEditJournalStoreStatus::IoFailure;
	if (size > kWtProductionJournalByteCapacity) {
		return WtEditJournalStoreStatus::CapacityExceeded;
	}
	if (size == 0) {
		journal_.reset(source_revision, initial_world_revision);
		path_ = path;
		open_ = true;
		return WtEditJournalStoreStatus::Ok;
	}
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
	std::ifstream input(path, std::ios::binary);
	if (!input || !input.read(
			reinterpret_cast<char *>(bytes.data()),
			static_cast<std::streamsize>(bytes.size())
		)) {
		return WtEditJournalStoreStatus::IoFailure;
	}
	std::size_t committed_bytes = 0;
	const WtEditJournalStatus status = journal_.load(
		{ bytes.data(), bytes.size() },
		source_revision,
		initial_world_revision,
		true,
		committed_bytes
	);
	if (status != WtEditJournalStatus::Ok &&
		status != WtEditJournalStatus::RecoveredTruncatedTail) {
		return map_journal_status(status);
	}
	if (status == WtEditJournalStatus::RecoveredTruncatedTail &&
		!resize_and_sync(path, committed_bytes)) {
		journal_.reset(source_revision, initial_world_revision);
		return WtEditJournalStoreStatus::IoFailure;
	}
	path_ = path;
	open_ = true;
	return WtEditJournalStoreStatus::Ok;
}

WtEditJournalStoreStatus WtEditJournalStore::append(
	const WtEditTransaction &transaction
) {
	if (!open_) return WtEditJournalStoreStatus::NotOpen;
	if (async_) {
		const WtEditJournalStoreStatus status = async_->flush();
		async_.reset();
		if (status != WtEditJournalStoreStatus::Ok) return status;
	}
	std::vector<std::uint8_t> segment;
	const WtEditJournalStatus prepare =
		journal_.prepare_append(transaction, segment);
	if (prepare != WtEditJournalStatus::Ok) {
		return map_journal_status(prepare);
	}
	std::error_code error;
	const bool exists = std::filesystem::exists(path_, error);
	if (error) return WtEditJournalStoreStatus::IoFailure;
	const std::uintmax_t previous_size = exists ?
		std::filesystem::file_size(path_, error) : 0;
	if (error || previous_size != journal_.byte_size()) {
		return WtEditJournalStoreStatus::IoFailure;
	}
	FILE *file = open_append(path_);
	if (file == nullptr) return WtEditJournalStoreStatus::IoFailure;
	const bool written = std::fwrite(
		segment.data(), 1, segment.size(), file
	) == segment.size();
	const bool durable = written && flush_durable(file);
	const bool closed = std::fclose(file) == 0;
	if (!durable || !closed) {
		resize_and_sync(path_, previous_size);
		return WtEditJournalStoreStatus::IoFailure;
	}
	const WtEditJournalStatus commit = journal_.commit_append({
		segment.data(), segment.size()
	});
	if (commit != WtEditJournalStatus::Ok) {
		resize_and_sync(path_, previous_size);
		return map_journal_status(commit);
	}
	return WtEditJournalStoreStatus::Ok;
}

WtEditJournalStoreStatus WtEditJournalStore::append_deferred(
	const WtEditTransaction &transaction
) {
	if (!open_) return WtEditJournalStoreStatus::NotOpen;
	std::vector<std::uint8_t> segment;
	const WtEditJournalStatus prepared =
		journal_.prepare_append(transaction, segment);
	if (prepared != WtEditJournalStatus::Ok) {
		return map_journal_status(prepared);
	}
	if (!async_) {
		std::error_code error;
		const bool exists = std::filesystem::exists(path_, error);
		const std::uintmax_t size = !error && exists ?
			std::filesystem::file_size(path_, error) : 0;
		if (error || size != journal_.byte_size()) {
			return WtEditJournalStoreStatus::IoFailure;
		}
		async_ = std::make_unique<AsyncState>(path_, size);
	}
	return async_->commit_and_enqueue(journal_, std::move(segment));
}

WtEditJournalStoreStatus WtEditJournalStore::flush_deferred() {
	return async_ ? async_->flush() : WtEditJournalStoreStatus::Ok;
}

WtEditJournalStoreStatus WtEditJournalStore::deferred_status() const noexcept {
	return async_ ? async_->status() : WtEditJournalStoreStatus::Ok;
}

void WtEditJournalStore::close() noexcept {
	async_.reset();
	open_ = false;
	path_.clear();
	journal_.reset(0, 0);
}

bool WtEditJournalStore::is_open() const noexcept {
	return open_;
}

const std::filesystem::path &WtEditJournalStore::path() const noexcept {
	return path_;
}

const WtEditJournal &WtEditJournalStore::journal() const noexcept {
	return journal_;
}

std::uint64_t WtEditJournalStore::current_world_revision() const noexcept {
	return open_ ? journal_.current_world_revision() : 0;
}

std::size_t WtEditJournalStore::transaction_count() const noexcept {
	return open_ ? journal_.transaction_count() : 0;
}

std::size_t WtEditJournalStore::command_count() const noexcept {
	return open_ ? journal_.command_count() : 0;
}

std::size_t WtEditJournalStore::byte_size() const noexcept {
	return open_ ? journal_.byte_size() : 0;
}

const char *wt_edit_journal_store_status_message(
	WtEditJournalStoreStatus status
) noexcept {
	switch (status) {
		case WtEditJournalStoreStatus::Ok: return "ok";
		case WtEditJournalStoreStatus::NotOpen:
			return "edit journal is not open";
		case WtEditJournalStoreStatus::InvalidPath:
			return "edit journal path is invalid";
		case WtEditJournalStoreStatus::IoFailure:
			return "edit journal I/O failed";
		case WtEditJournalStoreStatus::CapacityExceeded:
			return "edit journal capacity is exceeded";
		case WtEditJournalStoreStatus::CorruptJournal:
			return "edit journal is corrupt";
		case WtEditJournalStoreStatus::JournalFailure:
			return "edit journal transaction was rejected";
	}
	return "unknown edit journal status";
}

} // namespace world_transvoxel
