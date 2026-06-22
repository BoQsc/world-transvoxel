#pragma once

#include "editing/wt_edit_transaction.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace world_transvoxel {

enum class WtEditJournalStatus : std::uint8_t {
	Ok,
	RecoveredTruncatedTail,
	NotInitialized,
	InvalidInput,
	TransactionCapacityExceeded,
	CommandCapacityExceeded,
	ByteCapacityExceeded,
	SourceRevisionMismatch,
	WorldRevisionMismatch,
	DuplicateTransaction,
	DuplicateCommand,
	TransactionFailure,
	CorruptJournal,
	ReplayFailure,
};

class WtEditReplaySink {
public:
	virtual ~WtEditReplaySink() = default;
	virtual bool apply(const WtEditCommand &command) noexcept = 0;
};

class WtEditJournal {
public:
	WtEditJournal(
		std::size_t transaction_capacity,
		std::size_t command_capacity,
		std::size_t byte_capacity
	);

	void reset(
		std::uint64_t source_revision,
		std::uint64_t initial_world_revision
	);

	WtEditJournalStatus prepare_append(
		const WtEditTransaction &transaction,
		std::vector<std::uint8_t> &append_segment
	) const;

	WtEditJournalStatus commit_append(WtByteView append_segment);

	WtEditJournalStatus append(
		const WtEditTransaction &transaction,
		std::vector<std::uint8_t> &append_segment
	);

	WtEditJournalStatus load(
		WtByteView bytes,
		std::uint64_t expected_source_revision,
		std::uint64_t initial_world_revision,
		bool recover_truncated_tail,
		std::size_t &committed_bytes
	);

	WtEditJournalStatus save(std::vector<std::uint8_t> &output) const;
	WtEditJournalStatus replay(WtEditReplaySink &sink) const;
	WtEditJournalStatus replay_until(
		std::uint64_t maximum_revision,
		WtEditReplaySink &sink
	) const;

	bool initialized() const noexcept;
	std::uint64_t source_revision() const noexcept;
	std::uint64_t initial_world_revision() const noexcept;
	std::uint64_t current_world_revision() const noexcept;
	std::size_t transaction_count() const noexcept;
	std::size_t command_count() const noexcept;
	std::size_t byte_size() const noexcept;

private:
	WtEditJournalStatus validate_append(
		const WtEditTransaction &transaction,
		std::size_t segment_size
	) const;

	void commit_validated(
		const WtEditTransaction &transaction,
		std::vector<std::uint8_t> segment
	);

	void swap_state(WtEditJournal &other) noexcept;

	std::size_t transaction_capacity_ = 0;
	std::size_t command_capacity_ = 0;
	std::size_t byte_capacity_ = 0;
	bool initialized_ = false;
	std::uint64_t source_revision_ = 0;
	std::uint64_t initial_world_revision_ = 0;
	std::uint64_t current_world_revision_ = 0;
	std::size_t command_count_ = 0;
	std::size_t byte_size_ = 0;
	std::vector<WtEditTransaction> transactions_;
	std::vector<std::vector<std::uint8_t>> segments_;
	std::set<WtId128> transaction_ids_;
	std::set<WtId128> command_ids_;
};

} // namespace world_transvoxel
