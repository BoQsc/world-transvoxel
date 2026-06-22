#include "storage/wt_edit_journal_store.h"
#include "storage/wt_hash256.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
	wt::WtId128 output{};
	for (std::size_t index = 0; index < output.size(); ++index) {
		output[index] = static_cast<std::uint8_t>(seed + index);
	}
	return output;
}

wt::WtEditTransaction transaction(
	std::uint8_t seed,
	std::uint64_t base_revision
) {
	wt::WtEditTransaction output;
	output.source_revision = 7001;
	output.transaction_id = id(seed);
	output.base_revision = base_revision;
	output.committed_revision = base_revision + 1;
	output.author_id = 41;
	wt::WtEditCommand command;
	command.command_id = id(static_cast<std::uint8_t>(seed + 32));
	command.sequence = 0;
	command.world_revision = output.committed_revision;
	command.operation = wt::WtEditOperation::SetDensity;
	command.shape = wt::WtEditShape::AxisAlignedBox;
	command.density_value = 10.0F;
	command.box = {
		-2 * wt::kWtEditCoordinateScale,
		-2 * wt::kWtEditCoordinateScale,
		-2 * wt::kWtEditCoordinateScale,
		18 * wt::kWtEditCoordinateScale,
		18 * wt::kWtEditCoordinateScale,
		18 * wt::kWtEditCoordinateScale,
	};
	check(wt::wt_edit_box_bounds(command.box, command.bounds),
		"edit journal command bounds failed");
	output.commands.push_back(command);
	return output;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path &path) {
	std::ifstream input(path, std::ios::binary);
	return {
		std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>(),
	};
}

bool write_file(
	const std::filesystem::path &path,
	const std::vector<std::uint8_t> &bytes
) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write(
		reinterpret_cast<const char *>(bytes.data()),
		static_cast<std::streamsize>(bytes.size())
	);
	return output.good();
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

class FixtureRoot {
public:
	FixtureRoot() {
		path = std::filesystem::temp_directory_path() /
			("wt_production_edit_journal_" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()
			));
		std::filesystem::create_directories(path);
	}
	~FixtureRoot() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
	std::filesystem::path path;
};

} // namespace

int main() {
	FixtureRoot fixture;
	const std::filesystem::path journal_path = fixture.path / "world.wtedit";
	wt::WtEditJournalStore store;
	check(store.open(journal_path, 7001, 12) ==
		wt::WtEditJournalStoreStatus::Ok,
		"missing journal did not initialize");
	check(!std::filesystem::exists(journal_path) &&
		store.current_world_revision() == 12,
		"empty journal created bytes before the first commit");

	const wt::WtEditTransaction first = transaction(1, 12);
	check(store.append(first) == wt::WtEditJournalStoreStatus::Ok,
		"first durable journal append failed");
	const std::size_t first_size = store.byte_size();
	check(first_size != 0 &&
		std::filesystem::file_size(journal_path) == first_size &&
		store.current_world_revision() == 13 &&
		store.transaction_count() == 1 && store.command_count() == 1,
		"first durable journal state mismatch");
	const std::vector<std::uint8_t> first_bytes = read_file(journal_path);

	check(store.append(first) == wt::WtEditJournalStoreStatus::JournalFailure &&
		read_file(journal_path) == first_bytes,
		"duplicate journal append changed durable bytes");
	check(store.append(transaction(2, 12)) ==
		wt::WtEditJournalStoreStatus::JournalFailure &&
		read_file(journal_path) == first_bytes,
		"stale journal append changed durable bytes");

	store.close();
	check(store.open(journal_path, 7001, 12) ==
		wt::WtEditJournalStoreStatus::Ok &&
		store.current_world_revision() == 13 &&
		store.transaction_count() == 1,
		"journal restart load failed");
	check(store.append(transaction(2, 13)) ==
		wt::WtEditJournalStoreStatus::Ok,
		"second durable journal append failed");
	const std::vector<std::uint8_t> complete_bytes = read_file(journal_path);
	check(complete_bytes.size() > first_size &&
		store.current_world_revision() == 14 &&
		store.transaction_count() == 2,
		"second durable journal state mismatch");

	store.close();
	std::filesystem::resize_file(journal_path, complete_bytes.size() - 17);
	check(store.open(journal_path, 7001, 12) ==
		wt::WtEditJournalStoreStatus::Ok &&
		store.current_world_revision() == 13 &&
		store.transaction_count() == 1 &&
		std::filesystem::file_size(journal_path) == first_size &&
		read_file(journal_path) == first_bytes,
		"truncated journal tail did not recover and durably truncate");

	store.close();
	std::vector<std::uint8_t> corrupt = first_bytes;
	corrupt[corrupt.size() / 2] ^= 0x40U;
	check(write_file(journal_path, corrupt),
		"corrupt journal fixture write failed");
	check(store.open(journal_path, 7001, 12) ==
		wt::WtEditJournalStoreStatus::CorruptJournal &&
		!store.is_open(),
		"interior journal corruption was accepted");

	check(write_file(journal_path, first_bytes),
		"source-mismatch journal fixture write failed");
	check(store.open(journal_path, 9001, 12) ==
		wt::WtEditJournalStoreStatus::JournalFailure &&
		!store.is_open(),
		"journal source mismatch was accepted");

	if (failure_count != 0) {
		std::fprintf(stderr,
			"PRODUCTION_EDIT_JOURNAL_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("PRODUCTION_EDIT_JOURNAL_HASH ");
	print_hash(wt::wt_sha256(
		complete_bytes.data(), complete_bytes.size()
	));
	std::printf(
		"PRODUCTION_EDIT_JOURNAL_PASS commits=2 restart=1 "
		"truncated_recovery=1 corrupt_rejections=2 stale_rejections=2\n"
	);
	return 0;
}
