#include "editing/wt_edit_journal.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace world_transvoxel {

WtEditJournal::WtEditJournal(
	std::size_t transaction_capacity,
	std::size_t command_capacity,
	std::size_t byte_capacity
) :
		transaction_capacity_(transaction_capacity),
		command_capacity_(command_capacity),
		byte_capacity_(byte_capacity) {
	transactions_.reserve(transaction_capacity_);
	segments_.reserve(transaction_capacity_);
}

void WtEditJournal::reset(
	std::uint64_t source_revision,
	std::uint64_t initial_world_revision
) {
	initialized_ = true;
	source_revision_ = source_revision;
	initial_world_revision_ = initial_world_revision;
	current_world_revision_ = initial_world_revision;
	command_count_ = 0;
	byte_size_ = 0;
	transactions_.clear();
	segments_.clear();
	transaction_ids_.clear();
	command_ids_.clear();
}

WtEditJournalStatus WtEditJournal::validate_append(
	const WtEditTransaction &transaction,
	std::size_t segment_size
) const {
	if (!initialized_) return WtEditJournalStatus::NotInitialized;
	if (transaction.source_revision != source_revision_) {
		return WtEditJournalStatus::SourceRevisionMismatch;
	}
	if (transaction.base_revision != current_world_revision_ ||
		current_world_revision_ == std::numeric_limits<std::uint64_t>::max() ||
		transaction.committed_revision != current_world_revision_ + 1) {
		return WtEditJournalStatus::WorldRevisionMismatch;
	}
	if (transactions_.size() >= transaction_capacity_) {
		return WtEditJournalStatus::TransactionCapacityExceeded;
	}
	if (transaction.commands.size() > command_capacity_ - command_count_) {
		return WtEditJournalStatus::CommandCapacityExceeded;
	}
	if (segment_size > byte_capacity_ - byte_size_) {
		return WtEditJournalStatus::ByteCapacityExceeded;
	}
	if (transaction_ids_.find(transaction.transaction_id) !=
		transaction_ids_.end()) {
		return WtEditJournalStatus::DuplicateTransaction;
	}
	for (const WtEditCommand &command : transaction.commands) {
		if (command_ids_.find(command.command_id) != command_ids_.end()) {
			return WtEditJournalStatus::DuplicateCommand;
		}
	}
	return WtEditJournalStatus::Ok;
}

void WtEditJournal::commit_validated(
	const WtEditTransaction &transaction,
	std::vector<std::uint8_t> segment
) {
	transactions_.push_back(transaction);
	segments_.push_back(std::move(segment));
	transaction_ids_.insert(transaction.transaction_id);
	for (const WtEditCommand &command : transaction.commands) {
		command_ids_.insert(command.command_id);
	}
	current_world_revision_ = transaction.committed_revision;
	command_count_ += transaction.commands.size();
	byte_size_ += segments_.back().size();
}

WtEditJournalStatus WtEditJournal::prepare_append(
	const WtEditTransaction &transaction,
	std::vector<std::uint8_t> &append_segment
) const {
	append_segment.clear();
	std::vector<std::uint8_t> segment;
	if (wt_write_edit_transaction(transaction, segment) !=
		WtEditTransactionStatus::Ok) {
		return WtEditJournalStatus::TransactionFailure;
	}
	WtEditTransactionDocument document;
	if (wt_open_edit_transaction(
			{ segment.data(), segment.size() },
			document
		) != WtEditTransactionStatus::Ok) {
		return WtEditJournalStatus::TransactionFailure;
	}
	const WtEditJournalStatus status =
		validate_append(document.transaction, segment.size());
	if (status != WtEditJournalStatus::Ok) return status;
	append_segment = std::move(segment);
	return WtEditJournalStatus::Ok;
}

WtEditJournalStatus WtEditJournal::commit_append(WtByteView append_segment) {
	std::size_t measured_size = 0;
	if (wt_measure_container(
			append_segment,
			kWtEditMagic,
			measured_size
			) != WtContainerStatus::Ok ||
		measured_size != append_segment.size) {
		return WtEditJournalStatus::TransactionFailure;
	}
	WtEditTransactionDocument document;
	if (wt_open_edit_transaction(append_segment, document) !=
		WtEditTransactionStatus::Ok) {
		return WtEditJournalStatus::TransactionFailure;
	}
	std::vector<std::uint8_t> canonical;
	if (wt_write_edit_transaction(document.transaction, canonical) !=
			WtEditTransactionStatus::Ok ||
		canonical.size() != append_segment.size ||
		!std::equal(
			canonical.begin(),
			canonical.end(),
			append_segment.data
		)) {
		return WtEditJournalStatus::TransactionFailure;
	}
	const WtEditJournalStatus status =
		validate_append(document.transaction, canonical.size());
	if (status != WtEditJournalStatus::Ok) return status;
	commit_validated(document.transaction, std::move(canonical));
	return WtEditJournalStatus::Ok;
}

WtEditJournalStatus WtEditJournal::append(
	const WtEditTransaction &transaction,
	std::vector<std::uint8_t> &append_segment
) {
	const WtEditJournalStatus prepare_status =
		prepare_append(transaction, append_segment);
	if (prepare_status != WtEditJournalStatus::Ok) return prepare_status;
	const WtEditJournalStatus commit_status = commit_append({
		append_segment.data(),
		append_segment.size(),
	});
	if (commit_status != WtEditJournalStatus::Ok) {
		append_segment.clear();
	}
	return commit_status;
}

WtEditJournalStatus WtEditJournal::load(
	WtByteView bytes,
	std::uint64_t expected_source_revision,
	std::uint64_t initial_world_revision,
	bool recover_truncated_tail,
	std::size_t &committed_bytes
) {
	committed_bytes = 0;
	if (bytes.size != 0 && bytes.data == nullptr) {
		return WtEditJournalStatus::InvalidInput;
	}
	WtEditJournal replacement(
		transaction_capacity_,
		command_capacity_,
		byte_capacity_
	);
	replacement.reset(expected_source_revision, initial_world_revision);
	std::size_t offset = 0;
	while (offset < bytes.size) {
		std::size_t segment_size = 0;
		const WtContainerStatus measure_status = wt_measure_container(
			{ bytes.data + offset, bytes.size - offset },
			kWtEditMagic,
			segment_size
		);
		if (measure_status == WtContainerStatus::Truncated) {
			if (recover_truncated_tail) {
				committed_bytes = offset;
				swap_state(replacement);
				return WtEditJournalStatus::RecoveredTruncatedTail;
			}
			return WtEditJournalStatus::CorruptJournal;
		}
		if (measure_status != WtContainerStatus::Ok ||
			segment_size == 0) {
			return WtEditJournalStatus::CorruptJournal;
		}
		const WtByteView segment_view = {
			bytes.data + offset,
			segment_size,
		};
		WtEditTransactionDocument document;
		if (wt_open_edit_transaction(segment_view, document) !=
			WtEditTransactionStatus::Ok) {
			return WtEditJournalStatus::CorruptJournal;
		}
		std::vector<std::uint8_t> canonical;
		if (wt_write_edit_transaction(document.transaction, canonical) !=
				WtEditTransactionStatus::Ok ||
			canonical.size() != segment_view.size ||
			!std::equal(
				canonical.begin(),
				canonical.end(),
				segment_view.data
			)) {
			return WtEditJournalStatus::CorruptJournal;
		}
		const WtEditJournalStatus status =
			replacement.validate_append(document.transaction, canonical.size());
		if (status != WtEditJournalStatus::Ok) return status;
		replacement.commit_validated(
			document.transaction,
			std::move(canonical)
		);
		offset += segment_size;
	}
	committed_bytes = offset;
	swap_state(replacement);
	return WtEditJournalStatus::Ok;
}

WtEditJournalStatus WtEditJournal::save(
	std::vector<std::uint8_t> &output
) const {
	output.clear();
	if (!initialized_) return WtEditJournalStatus::NotInitialized;
	output.reserve(byte_size_);
	for (const std::vector<std::uint8_t> &segment : segments_) {
		output.insert(output.end(), segment.begin(), segment.end());
	}
	return output.size() == byte_size_ ?
		WtEditJournalStatus::Ok :
		WtEditJournalStatus::CorruptJournal;
}

WtEditJournalStatus WtEditJournal::replay(WtEditReplaySink &sink) const {
	return replay_until(current_world_revision_, sink);
}

WtEditJournalStatus WtEditJournal::replay_until(
	std::uint64_t maximum_revision,
	WtEditReplaySink &sink
) const {
	if (!initialized_) return WtEditJournalStatus::NotInitialized;
	if (maximum_revision < initial_world_revision_ ||
		maximum_revision > current_world_revision_) {
		return WtEditJournalStatus::WorldRevisionMismatch;
	}
	for (const WtEditTransaction &transaction : transactions_) {
		if (transaction.committed_revision > maximum_revision) break;
		for (const WtEditCommand &command : transaction.commands) {
			if (!sink.apply(command)) {
				return WtEditJournalStatus::ReplayFailure;
			}
		}
	}
	return WtEditJournalStatus::Ok;
}

void WtEditJournal::swap_state(WtEditJournal &other) noexcept {
	std::swap(initialized_, other.initialized_);
	std::swap(source_revision_, other.source_revision_);
	std::swap(initial_world_revision_, other.initial_world_revision_);
	std::swap(current_world_revision_, other.current_world_revision_);
	std::swap(command_count_, other.command_count_);
	std::swap(byte_size_, other.byte_size_);
	transactions_.swap(other.transactions_);
	segments_.swap(other.segments_);
	transaction_ids_.swap(other.transaction_ids_);
	command_ids_.swap(other.command_ids_);
}

bool WtEditJournal::initialized() const noexcept {
	return initialized_;
}

std::uint64_t WtEditJournal::source_revision() const noexcept {
	return source_revision_;
}

std::uint64_t WtEditJournal::initial_world_revision() const noexcept {
	return initial_world_revision_;
}

std::uint64_t WtEditJournal::current_world_revision() const noexcept {
	return current_world_revision_;
}

std::size_t WtEditJournal::transaction_count() const noexcept {
	return transactions_.size();
}

std::size_t WtEditJournal::command_count() const noexcept {
	return command_count_;
}

std::size_t WtEditJournal::byte_size() const noexcept {
	return byte_size_;
}

} // namespace world_transvoxel
