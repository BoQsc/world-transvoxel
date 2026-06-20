#include "editing/wt_edit_journal.h"
#include "storage/wt_hash256.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace wt = world_transvoxel;

namespace {

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

wt::WtId128 id(std::uint8_t seed) {
	wt::WtId128 value{};
	for (std::size_t index = 0; index < value.size(); ++index) {
		value[index] = static_cast<std::uint8_t>(seed + index);
	}
	return value;
}

wt::WtEditCommand command(
	std::uint8_t seed,
	std::uint32_t sequence,
	std::uint64_t revision,
	std::int64_t center
) {
	wt::WtEditCommand output;
	output.command_id = id(seed);
	output.sequence = sequence;
	output.world_revision = revision;
	output.operation = wt::WtEditOperation::AddDensity;
	output.shape = wt::WtEditShape::Sphere;
	output.density_value = -0.5F;
	output.sphere = {
		center * wt::kWtEditCoordinateScale,
		0,
		0,
		static_cast<std::uint64_t>(wt::kWtEditCoordinateScale / 2),
	};
	check(
		wt::wt_edit_sphere_bounds(output.sphere, output.bounds),
		"journal command bounds failed"
	);
	return output;
}

wt::WtEditTransaction transaction(
	std::uint8_t seed,
	std::uint64_t base_revision,
	std::uint32_t command_count
) {
	wt::WtEditTransaction output;
	output.source_revision = 7001;
	output.transaction_id = id(seed);
	output.base_revision = base_revision;
	output.committed_revision = base_revision + 1;
	output.author_id = 42;
	for (std::uint32_t index = 0; index < command_count; ++index) {
		output.commands.push_back(command(
			static_cast<std::uint8_t>(seed + 20 + index * 20),
			index,
			output.committed_revision,
			static_cast<std::int64_t>(base_revision * 10 + index)
		));
	}
	std::reverse(output.commands.begin(), output.commands.end());
	return output;
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

class RecordingSink final : public wt::WtEditReplaySink {
public:
	explicit RecordingSink(std::size_t fail_after = 1000) :
			fail_after_(fail_after) {
	}

	bool apply(const wt::WtEditCommand &value) noexcept override {
		if (commands.size() >= fail_after_) return false;
		commands.push_back(value);
		return true;
	}

	std::vector<wt::WtEditCommand> commands;

private:
	std::size_t fail_after_ = 0;
};

std::vector<std::uint8_t> build_journal(
	std::vector<std::size_t> &segment_sizes
) {
	wt::WtEditJournal journal(3, 6, 8192);
	journal.reset(7001, 10);
	std::vector<std::uint8_t> appended;
	for (std::uint64_t index = 0; index < 3; ++index) {
		const wt::WtEditTransaction value = transaction(
			static_cast<std::uint8_t>(1 + index),
			10 + index,
			2
		);
		check(
			journal.append(value, appended) == wt::WtEditJournalStatus::Ok,
			"journal append failed"
		);
		segment_sizes.push_back(appended.size());
	}
	check(
		journal.initialized() &&
			journal.source_revision() == 7001 &&
			journal.initial_world_revision() == 10 &&
			journal.current_world_revision() == 13 &&
			journal.transaction_count() == 3 &&
			journal.command_count() == 6,
		"journal state after append mismatch"
	);
	std::vector<std::uint8_t> saved;
	check(
		journal.save(saved) == wt::WtEditJournalStatus::Ok &&
			saved.size() == journal.byte_size(),
		"journal save failed"
	);
	check(
		saved.size() ==
			segment_sizes[0] + segment_sizes[1] + segment_sizes[2],
		"journal segment sizes do not compose"
	);
	return saved;
}

void test_load_and_replay(
	const std::vector<std::uint8_t> &bytes,
	const std::vector<std::size_t> &segment_sizes
) {
	std::size_t first_size = 0;
	check(
		wt::wt_measure_container(
			{ bytes.data(), bytes.size() },
			wt::kWtEditMagic,
			first_size
		) == wt::WtContainerStatus::Ok &&
			first_size == segment_sizes[0],
		"container prefix measurement failed"
	);

	wt::WtEditJournal loaded(3, 6, 8192);
	std::size_t committed_bytes = 0;
	check(
		loaded.load(
			{ bytes.data(), bytes.size() },
			7001,
			10,
			false,
			committed_bytes
		) == wt::WtEditJournalStatus::Ok &&
			committed_bytes == bytes.size(),
		"journal load failed"
	);
	std::vector<std::uint8_t> resaved;
	check(
		loaded.save(resaved) == wt::WtEditJournalStatus::Ok &&
			resaved == bytes,
		"loaded journal changed bytes"
	);

	RecordingSink sink;
	check(
		loaded.replay(sink) == wt::WtEditJournalStatus::Ok &&
			sink.commands.size() == 6,
		"journal replay failed"
	);
	for (std::size_t index = 0; index < sink.commands.size(); ++index) {
		check(
			sink.commands[index].sequence == index % 2 &&
				sink.commands[index].world_revision == 11 + index / 2,
			"journal replay order mismatch"
		);
	}
	RecordingSink failing_sink(3);
	check(
		loaded.replay(failing_sink) == wt::WtEditJournalStatus::ReplayFailure &&
			failing_sink.commands.size() == 3,
		"journal replay sink failure was ignored"
	);
}

void test_truncated_tail(
	const std::vector<std::uint8_t> &bytes,
	const std::vector<std::size_t> &segment_sizes
) {
	std::vector<std::uint8_t> truncated = bytes;
	truncated.resize(truncated.size() - 17);

	wt::WtEditJournal strict(3, 6, 8192);
	strict.reset(7001, 77);
	std::size_t committed_bytes = 0;
	check(
		strict.load(
			{ truncated.data(), truncated.size() },
			7001,
			10,
			false,
			committed_bytes
		) == wt::WtEditJournalStatus::CorruptJournal &&
			strict.current_world_revision() == 77 &&
			strict.transaction_count() == 0,
		"strict truncated-tail load mutated journal"
	);

	wt::WtEditJournal recovered(3, 6, 8192);
	check(
		recovered.load(
			{ truncated.data(), truncated.size() },
			7001,
			10,
			true,
			committed_bytes
		) == wt::WtEditJournalStatus::RecoveredTruncatedTail &&
			committed_bytes == segment_sizes[0] + segment_sizes[1] &&
			recovered.transaction_count() == 2 &&
			recovered.command_count() == 4 &&
			recovered.current_world_revision() == 12,
		"truncated-tail recovery did not retain committed prefix"
	);
	std::vector<std::uint8_t> prefix;
	check(
		recovered.save(prefix) == wt::WtEditJournalStatus::Ok &&
			prefix.size() == committed_bytes &&
			std::equal(prefix.begin(), prefix.end(), bytes.begin()),
		"recovered journal prefix mismatch"
	);
}

void test_corruption_is_atomic(
	const std::vector<std::uint8_t> &bytes,
	const std::vector<std::size_t> &segment_sizes
) {
	std::vector<std::uint8_t> corrupted = bytes;
	corrupted[segment_sizes[0] + segment_sizes[1] / 2] ^= 0x40U;
	wt::WtEditJournal journal(3, 6, 8192);
	journal.reset(7001, 55);
	std::size_t committed_bytes = 0;
	check(
		journal.load(
			{ corrupted.data(), corrupted.size() },
			7001,
			10,
			true,
			committed_bytes
		) == wt::WtEditJournalStatus::CorruptJournal &&
			journal.current_world_revision() == 55 &&
			journal.transaction_count() == 0,
		"complete-segment corruption was recovered or mutated state"
	);
}

void test_append_failures() {
	std::vector<std::uint8_t> segment = { 1 };
	wt::WtEditJournal journal(2, 3, 4096);
	check(
		journal.append(transaction(1, 10, 1), segment) ==
			wt::WtEditJournalStatus::NotInitialized &&
			segment.empty(),
		"uninitialized append succeeded"
	);
	journal.reset(7001, 10);
	check(
		journal.prepare_append(transaction(1, 10, 1), segment) ==
			wt::WtEditJournalStatus::Ok &&
			!segment.empty() &&
			journal.current_world_revision() == 10 &&
			journal.transaction_count() == 0,
		"prepared append mutated journal state"
	);
	check(
		journal.commit_append({ segment.data(), segment.size() }) ==
			wt::WtEditJournalStatus::Ok &&
			journal.current_world_revision() == 11 &&
			journal.transaction_count() == 1,
		"prepared segment commit failed"
	);
	check(
		journal.commit_append({ segment.data(), segment.size() }) ==
			wt::WtEditJournalStatus::WorldRevisionMismatch,
		"already committed segment was accepted again"
	);

	wt::WtEditTransaction wrong_source = transaction(2, 11, 1);
	wrong_source.source_revision = 7002;
	check(
		journal.append(wrong_source, segment) ==
			wt::WtEditJournalStatus::SourceRevisionMismatch &&
			segment.empty(),
		"source revision mismatch was accepted"
	);
	check(
		journal.append(transaction(2, 12, 1), segment) ==
			wt::WtEditJournalStatus::WorldRevisionMismatch,
		"world revision gap was accepted"
	);

	wt::WtEditTransaction duplicate_transaction = transaction(1, 11, 1);
	duplicate_transaction.commands[0].command_id = id(90);
	check(
		journal.append(duplicate_transaction, segment) ==
			wt::WtEditJournalStatus::DuplicateTransaction,
		"duplicate transaction ID was accepted"
	);
	wt::WtEditTransaction duplicate_command = transaction(2, 11, 1);
	duplicate_command.commands[0].command_id =
		transaction(1, 10, 1).commands[0].command_id;
	check(
		journal.append(duplicate_command, segment) ==
			wt::WtEditJournalStatus::DuplicateCommand,
		"duplicate command ID was accepted"
	);

	check(
		journal.append(transaction(2, 11, 2), segment) ==
			wt::WtEditJournalStatus::Ok,
		"append failure fixture second append failed"
	);
	check(
		journal.append(transaction(3, 12, 1), segment) ==
			wt::WtEditJournalStatus::TransactionCapacityExceeded,
		"transaction capacity overflow was accepted"
	);

	wt::WtEditJournal command_limited(3, 1, 4096);
	command_limited.reset(7001, 10);
	check(
		command_limited.append(transaction(1, 10, 2), segment) ==
			wt::WtEditJournalStatus::CommandCapacityExceeded,
		"command capacity overflow was accepted"
	);
	wt::WtEditJournal byte_limited(3, 3, 1);
	byte_limited.reset(7001, 10);
	check(
		byte_limited.append(transaction(1, 10, 1), segment) ==
			wt::WtEditJournalStatus::ByteCapacityExceeded,
		"byte capacity overflow was accepted"
	);
}

} // namespace

int main() {
	std::vector<std::size_t> segment_sizes;
	const std::vector<std::uint8_t> bytes = build_journal(segment_sizes);
	test_load_and_replay(bytes, segment_sizes);
	test_truncated_tail(bytes, segment_sizes);
	test_corruption_is_atomic(bytes, segment_sizes);
	test_append_failures();
	if (failure_count != 0) {
		std::fprintf(stderr, "M4_JOURNAL_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M4_JOURNAL_HASH ");
	print_hash(wt::wt_sha256(bytes.data(), bytes.size()));
	std::printf("M4_JOURNAL_PASS transactions=3 commands=6 failure_cases=12\n");
	return 0;
}
