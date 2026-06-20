#include "editing/wt_edit_transaction.h"
#include "storage/wt_hash256.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
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

wt::WtEditCommand sphere_command(
	std::uint32_t sequence,
	wt::WtEditOperation operation,
	float density
) {
	wt::WtEditCommand command;
	command.command_id = id(static_cast<std::uint8_t>(20 + sequence * 20));
	command.sequence = sequence;
	command.world_revision = 42;
	command.operation = operation;
	command.shape = wt::WtEditShape::Sphere;
	command.density_value = density;
	command.sphere = {
		-wt::kWtEditCoordinateScale / 2,
		wt::kWtEditCoordinateScale / 2,
		2 * wt::kWtEditCoordinateScale,
		static_cast<std::uint64_t>(
			wt::kWtEditCoordinateScale +
			wt::kWtEditCoordinateScale / 4
		),
	};
	check(
		wt::wt_edit_sphere_bounds(command.sphere, command.bounds),
		"sphere bounds construction failed"
	);
	return command;
}

wt::WtEditCommand box_command(std::uint32_t sequence) {
	wt::WtEditCommand command;
	command.command_id = id(static_cast<std::uint8_t>(20 + sequence * 20));
	command.sequence = sequence;
	command.world_revision = 42;
	command.operation = wt::WtEditOperation::PaintMaterial;
	command.shape = wt::WtEditShape::AxisAlignedBox;
	command.material = 37;
	command.box = {
		-wt::kWtEditCoordinateScale -
			wt::kWtEditCoordinateScale / 4,
		0,
		wt::kWtEditCoordinateScale / 4,
		2 * wt::kWtEditCoordinateScale +
			wt::kWtEditCoordinateScale / 2,
		3 * wt::kWtEditCoordinateScale,
		wt::kWtEditCoordinateScale +
			wt::kWtEditCoordinateScale / 2,
	};
	check(
		wt::wt_edit_box_bounds(command.box, command.bounds),
		"box bounds construction failed"
	);
	return command;
}

wt::WtEditTransaction make_transaction() {
	wt::WtEditTransaction transaction;
	transaction.source_revision = 7001;
	transaction.transaction_id = id(1);
	transaction.base_revision = 41;
	transaction.committed_revision = 42;
	transaction.author_id = 9001;
	transaction.commands = {
		box_command(2),
		sphere_command(0, wt::WtEditOperation::AddDensity, -0.75F),
		sphere_command(1, wt::WtEditOperation::SetDensity, 1.25F),
	};
	return transaction;
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void test_bounds() {
	const wt::WtEditCommand sphere =
		sphere_command(0, wt::WtEditOperation::AddDensity, -1.0F);
	check(
		sphere.bounds == wt::WtEditBounds{
			{ -2, -1, 0 },
			{ 1, 2, 4 },
		},
		"sphere conservative bounds mismatch"
	);
	const wt::WtEditCommand box = box_command(1);
	check(
		box.bounds == wt::WtEditBounds{
			{ -2, 0, 0 },
			{ 3, 3, 2 },
		},
		"box conservative bounds mismatch"
	);
	wt::WtEditSphere overflow;
	overflow.center_x_q16 = std::numeric_limits<std::int64_t>::max();
	overflow.radius_q16 = 1;
	wt::WtEditBounds bounds;
	check(
		!wt::wt_edit_sphere_bounds(overflow, bounds),
		"overflowing sphere bounds were accepted"
	);
}

void test_round_trip(std::vector<std::uint8_t> &bytes) {
	wt::WtEditTransaction first = make_transaction();
	wt::WtEditTransaction second = first;
	std::reverse(second.commands.begin(), second.commands.end());
	std::vector<std::uint8_t> reordered;
	check(
		wt::wt_write_edit_transaction(first, bytes) ==
			wt::WtEditTransactionStatus::Ok,
		"edit transaction write failed"
	);
	check(
		wt::wt_write_edit_transaction(second, reordered) ==
			wt::WtEditTransactionStatus::Ok,
		"reordered edit transaction write failed"
	);
	check(bytes == reordered, "edit transaction is input-order dependent");

	wt::WtEditTransactionDocument document;
	check(
		wt::wt_open_edit_transaction(
			{ bytes.data(), bytes.size() },
			document
		) == wt::WtEditTransactionStatus::Ok,
		"edit transaction open failed"
	);
	check(
		document.transaction.source_revision == 7001 &&
			document.transaction.transaction_id == id(1) &&
			document.transaction.base_revision == 41 &&
			document.transaction.committed_revision == 42 &&
			document.transaction.author_id == 9001 &&
			document.transaction.commands.size() == 3,
		"edit transaction header mismatch"
	);
	if (document.transaction.commands.size() == 3) {
		check(
			document.transaction.commands[0].sequence == 0 &&
				document.transaction.commands[0].operation ==
					wt::WtEditOperation::AddDensity &&
				document.transaction.commands[1].sequence == 1 &&
				document.transaction.commands[1].operation ==
					wt::WtEditOperation::SetDensity &&
				document.transaction.commands[2].sequence == 2 &&
				document.transaction.commands[2].operation ==
					wt::WtEditOperation::PaintMaterial,
			"edit command order or operation mismatch"
		);
		check(
			document.transaction.commands[2].material == 37 &&
				document.transaction.commands[2].bounds ==
					box_command(2).bounds,
			"edit command payload mismatch"
		);
	}
	std::vector<std::uint8_t> rewritten;
	check(
		wt::wt_write_edit_transaction(document.transaction, rewritten) ==
			wt::WtEditTransactionStatus::Ok &&
			rewritten == bytes,
		"edit transaction round trip changed bytes"
	);
}

void expect_invalid(
	wt::WtEditTransaction transaction,
	const char *message
) {
	std::vector<std::uint8_t> bytes;
	check(
		wt::wt_write_edit_transaction(transaction, bytes) ==
			wt::WtEditTransactionStatus::InvalidInput &&
			bytes.empty(),
		message
	);
}

void test_write_failures() {
	wt::WtEditTransaction transaction = make_transaction();
	transaction.committed_revision = 43;
	expect_invalid(transaction, "non-contiguous transaction revision accepted");

	transaction = make_transaction();
	transaction.commands[1].command_id = transaction.commands[0].command_id;
	expect_invalid(transaction, "duplicate command ID accepted");

	transaction = make_transaction();
	transaction.commands[1].sequence = 8;
	expect_invalid(transaction, "non-contiguous command sequence accepted");

	transaction = make_transaction();
	transaction.commands[1].world_revision = 43;
	expect_invalid(transaction, "command world revision mismatch accepted");

	transaction = make_transaction();
	transaction.commands[1].bounds.maximum.x += 1;
	expect_invalid(transaction, "incorrect conservative bounds accepted");

	transaction = make_transaction();
	transaction.commands[1].density_value =
		std::numeric_limits<float>::infinity();
	expect_invalid(transaction, "non-finite density value accepted");

	transaction = make_transaction();
	transaction.commands[1].density_value = 0.0F;
	expect_invalid(transaction, "zero-strength additive edit accepted");

	transaction = make_transaction();
	transaction.commands[0].density_value = 1.0F;
	expect_invalid(transaction, "paint command density value accepted");

	transaction = make_transaction();
	transaction.commands[0].box.minimum_x_q16 =
		transaction.commands[0].box.maximum_x_q16 + 1;
	expect_invalid(transaction, "inverted box accepted");
}

void test_read_failures(const std::vector<std::uint8_t> &valid_bytes) {
	std::vector<std::uint8_t> corrupted = valid_bytes;
	corrupted.back() ^= 0x20U;
	wt::WtEditTransactionDocument document;
	check(
		wt::wt_open_edit_transaction(
			{ corrupted.data(), corrupted.size() },
			document
		) == wt::WtEditTransactionStatus::ContainerFailure,
		"corrupted edit container was accepted"
	);

	wt::WtEditTransactionDocument valid;
	check(
		wt::wt_open_edit_transaction(
			{ valid_bytes.data(), valid_bytes.size() },
			valid
		) == wt::WtEditTransactionStatus::Ok,
		"edit read-failure fixture failed"
	);
	const wt::WtContainerSection *header =
		valid.container.find_section(wt::kWtEditHeaderSection);
	const wt::WtContainerSection *commands =
		valid.container.find_section(wt::kWtEditCommandSection);
	const wt::WtContainerSection *commit =
		valid.container.find_section(wt::kWtEditCommitSection);
	if (header == nullptr || commands == nullptr || commit == nullptr) {
		check(false, "edit fixture sections missing");
		return;
	}
	std::vector<std::uint8_t> bytes;
	std::vector<wt::WtContainerSectionInput> sections = {
		{ wt::kWtEditHeaderSection, 1, wt::WtStorageCodec::None,
			header->payload },
		{ wt::kWtEditCommandSection, 0, wt::WtStorageCodec::None,
			commands->payload },
		{ wt::kWtEditCommitSection, 0, wt::WtStorageCodec::None,
			commit->payload },
	};
	check(
		wt::wt_write_container(
			wt::kWtEditMagic,
			0,
			7001,
			sections,
			bytes
		) == wt::WtContainerStatus::Ok &&
			wt::wt_open_edit_transaction(
				{ bytes.data(), bytes.size() },
				document
			) == wt::WtEditTransactionStatus::InvalidTransaction,
		"nonzero edit section flags were accepted"
	);

	wt::WtEditTransaction changed = make_transaction();
	changed.commands[1].density_value = 2.0F;
	std::vector<std::uint8_t> changed_bytes;
	wt::WtEditTransactionDocument changed_document;
	check(
		wt::wt_write_edit_transaction(changed, changed_bytes) ==
			wt::WtEditTransactionStatus::Ok &&
			wt::wt_open_edit_transaction(
				{ changed_bytes.data(), changed_bytes.size() },
				changed_document
			) == wt::WtEditTransactionStatus::Ok,
		"changed edit fixture failed"
	);
	const wt::WtContainerSection *changed_commands =
		changed_document.container.find_section(wt::kWtEditCommandSection);
	if (changed_commands == nullptr) {
		check(false, "changed command section missing");
		return;
	}
	sections[0].flags = 0;
	sections[1].payload = changed_commands->payload;
	check(
		wt::wt_write_container(
			wt::kWtEditMagic,
			0,
			7001,
			sections,
			bytes
		) == wt::WtContainerStatus::Ok &&
			wt::wt_open_edit_transaction(
				{ bytes.data(), bytes.size() },
				document
			) == wt::WtEditTransactionStatus::HashMismatch,
		"commit-to-command hash mismatch was accepted"
	);
}

} // namespace

int main() {
	test_bounds();
	std::vector<std::uint8_t> bytes;
	test_round_trip(bytes);
	test_write_failures();
	test_read_failures(bytes);
	if (failure_count != 0) {
		std::fprintf(stderr, "M4_EDIT_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M4_EDIT_HASH ");
	print_hash(wt::wt_sha256(bytes.data(), bytes.size()));
	std::printf("M4_EDIT_PASS commands=3 failure_cases=14\n");
	return 0;
}
