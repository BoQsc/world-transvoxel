#pragma once

#include "editing/wt_edit_journal.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace world_transvoxel {

constexpr std::size_t kWtProductionJournalTransactionCapacity = 4096;
constexpr std::size_t kWtProductionJournalCommandCapacity = 65536;
constexpr std::size_t kWtProductionJournalByteCapacity =
	64U * 1024U * 1024U;

enum class WtEditJournalStoreStatus : std::uint8_t {
	Ok,
	NotOpen,
	InvalidPath,
	IoFailure,
	CapacityExceeded,
	CorruptJournal,
	JournalFailure,
};

class WtEditJournalStore {
public:
	WtEditJournalStore();

	WtEditJournalStoreStatus open(
		const std::filesystem::path &path,
		std::uint64_t source_revision,
		std::uint64_t initial_world_revision
	);
	WtEditJournalStoreStatus append(
		const WtEditTransaction &transaction
	);
	void close() noexcept;

	bool is_open() const noexcept;
	const std::filesystem::path &path() const noexcept;
	const WtEditJournal &journal() const noexcept;
	std::uint64_t current_world_revision() const noexcept;
	std::size_t transaction_count() const noexcept;
	std::size_t command_count() const noexcept;
	std::size_t byte_size() const noexcept;

private:
	WtEditJournal journal_;
	std::filesystem::path path_;
	bool open_ = false;
};

const char *wt_edit_journal_store_status_message(
	WtEditJournalStoreStatus status
) noexcept;

} // namespace world_transvoxel
