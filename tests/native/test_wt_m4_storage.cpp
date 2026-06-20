#include "storage/wt_binary_io.h"
#include "storage/wt_container_format.h"
#include "storage/wt_hash256.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

std::uint8_t hex_nibble(char value) {
	if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
	if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
	return static_cast<std::uint8_t>(value - 'A' + 10);
}

wt::WtHash256 parse_hash(const char *hex) {
	wt::WtHash256 hash{};
	for (std::size_t index = 0; index < hash.size(); ++index) {
		hash[index] = static_cast<std::uint8_t>(
			(hex_nibble(hex[index * 2]) << 4) | hex_nibble(hex[index * 2 + 1])
		);
	}
	return hash;
}

void patch_u16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint16_t value) {
	bytes[offset] = static_cast<std::uint8_t>(value);
	bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void patch_u64(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint64_t value) {
	for (unsigned int index = 0; index < 8; ++index) {
		bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
	}
}

void repair_payload_hash(std::vector<std::uint8_t> &bytes) {
	const wt::WtHash256 hash = wt::wt_sha256(
		bytes.data() + wt::kWtContainerHeaderSize,
		bytes.size() - wt::kWtContainerHeaderSize
	);
	std::memcpy(bytes.data() + 48, hash.data(), hash.size());
}

void test_sha256() {
	check(
		wt::wt_sha256(nullptr, 0) ==
			parse_hash("e3b0c44298fc1c149afbf4c8996fb924"
				"27ae41e4649b934ca495991b7852b855"),
		"empty SHA-256 vector mismatch"
	);
	const std::array<std::uint8_t, 3> abc = { 'a', 'b', 'c' };
	check(
		wt::wt_sha256(abc.data(), abc.size()) ==
			parse_hash("ba7816bf8f01cfea414140de5dae2223"
				"b00361a396177a9cb410ff61f20015ad"),
		"abc SHA-256 vector mismatch"
	);
}

void test_binary_io() {
	wt::WtBinaryWriter writer(23);
	check(writer.write_u8(0x12) == wt::WtBinaryStatus::Ok, "write u8 failed");
	check(writer.write_u16(0x3456) == wt::WtBinaryStatus::Ok, "write u16 failed");
	check(writer.write_u32(0x789abcdeU) == wt::WtBinaryStatus::Ok, "write u32 failed");
	check(writer.write_u64(0x0123456789abcdefULL) == wt::WtBinaryStatus::Ok,
		"write u64 failed");
	check(writer.write_i64(-7) == wt::WtBinaryStatus::Ok, "write i64 failed");
	check(writer.write_u8(1) == wt::WtBinaryStatus::CapacityExceeded,
		"writer accepted capacity overflow");

	const std::vector<std::uint8_t> &bytes = writer.bytes();
	wt::WtBinaryReader reader({ bytes.data(), bytes.size() });
	std::uint8_t u8 = 0;
	std::uint16_t u16 = 0;
	std::uint32_t u32 = 0;
	std::uint64_t u64 = 0;
	std::int64_t i64 = 0;
	check(reader.read_u8(u8) == wt::WtBinaryStatus::Ok && u8 == 0x12,
		"read u8 mismatch");
	check(reader.read_u16(u16) == wt::WtBinaryStatus::Ok && u16 == 0x3456,
		"read u16 mismatch");
	check(reader.read_u32(u32) == wt::WtBinaryStatus::Ok && u32 == 0x789abcdeU,
		"read u32 mismatch");
	check(reader.read_u64(u64) == wt::WtBinaryStatus::Ok &&
		u64 == 0x0123456789abcdefULL, "read u64 mismatch");
	check(reader.read_i64(i64) == wt::WtBinaryStatus::Ok && i64 == -7,
		"read i64 mismatch");
	check(reader.read_u8(u8) == wt::WtBinaryStatus::OutOfBounds,
		"reader accepted truncation");
}

std::vector<std::uint8_t> write_fixture() {
	const std::array<std::uint8_t, 5> metadata = { 1, 2, 3, 4, 5 };
	const std::array<std::uint8_t, 4> dependencies = { 9, 8, 7, 6 };
	const std::vector<wt::WtContainerSectionInput> sections = {
		{ wt::wt_fourcc('D', 'E', 'P', 'S'), 7, wt::WtStorageCodec::None,
			{ dependencies.data(), dependencies.size() } },
		{ wt::wt_fourcc('M', 'E', 'T', 'A'), 3, wt::WtStorageCodec::None,
			{ metadata.data(), metadata.size() } },
	};
	std::vector<std::uint8_t> output;
	check(wt::wt_write_container(
		wt::kWtWorldMagic, 0x1234, 42, sections, output
	) == wt::WtContainerStatus::Ok, "container write failed");
	return output;
}

void test_container_round_trip() {
	const std::vector<std::uint8_t> first = write_fixture();
	const std::vector<std::uint8_t> second = write_fixture();
	check(first == second, "container output is not deterministic");
	check(first.size() == 80 + 2 * 68 + 9, "container size mismatch");

	wt::WtContainerView view;
	check(wt::wt_read_container(
		{ first.data(), first.size() }, wt::kWtWorldMagic, view
	) == wt::WtContainerStatus::Ok, "container read failed");
	check(view.header.format_major == 1 && view.header.format_minor == 0 &&
		view.header.source_revision == 42 && view.header.feature_flags == 0x1234,
		"container header mismatch");
	const wt::WtContainerSection *dependencies =
		view.find_section(wt::wt_fourcc('D', 'E', 'P', 'S'));
	const wt::WtContainerSection *metadata =
		view.find_section(wt::wt_fourcc('M', 'E', 'T', 'A'));
	check(dependencies != nullptr && dependencies->payload.size == 4 &&
		dependencies->payload.data[0] == 9 && dependencies->flags == 7,
		"dependency section mismatch");
	check(metadata != nullptr && metadata->payload.size == 5 &&
		metadata->payload.data[4] == 5 && metadata->flags == 3,
		"metadata section mismatch");
	check(metadata != nullptr && metadata->payload.data >= first.data() &&
		metadata->payload.data < first.data() + first.size(),
		"section payload was copied instead of viewed");
	check(view.find_section(wt::wt_fourcc('N', 'O', 'N', 'E')) == nullptr,
		"missing section lookup succeeded");
}

void expect_failure(
	std::vector<std::uint8_t> bytes,
	wt::WtContainerStatus expected,
	const char *message,
	bool repair_hash = false
) {
	if (repair_hash) {
		repair_payload_hash(bytes);
	}
	wt::WtContainerView view;
	check(wt::wt_read_container(
		{ bytes.data(), bytes.size() }, wt::kWtWorldMagic, view
	) == expected, message);
	check(view.sections.empty(), "failed read retained section views");
}

void test_container_corruption() {
	const std::vector<std::uint8_t> valid = write_fixture();
	std::vector<std::uint8_t> bytes = valid;
	bytes.pop_back();
	expect_failure(bytes, wt::WtContainerStatus::HashMismatch, "truncation was not rejected");

	bytes = valid;
	bytes[0] = 'X';
	expect_failure(bytes, wt::WtContainerStatus::InvalidMagic, "invalid magic accepted");

	bytes = valid;
	patch_u16(bytes, 8, 2);
	expect_failure(bytes, wt::WtContainerStatus::UnsupportedVersion,
		"future major version accepted");

	bytes = valid;
	patch_u64(bytes, 16, 1ULL << 63);
	expect_failure(bytes, wt::WtContainerStatus::UnsupportedRequiredFeature,
		"unknown required feature accepted");

	bytes = valid;
	patch_u16(bytes, 80 + 8, 1);
	expect_failure(bytes, wt::WtContainerStatus::UnsupportedCodec,
		"unknown codec accepted", true);

	bytes = valid;
	patch_u64(bytes, 80 + 12, valid.size() + 1);
	expect_failure(bytes, wt::WtContainerStatus::InvalidSectionBounds,
		"out-of-range section accepted", true);

	bytes = valid;
	std::memcpy(bytes.data() + 80 + 68, bytes.data() + 80, 4);
	expect_failure(bytes, wt::WtContainerStatus::DuplicateSection,
		"duplicate section accepted", true);

	bytes = valid;
	bytes.back() ^= 0x80;
	expect_failure(bytes, wt::WtContainerStatus::HashMismatch,
		"payload corruption accepted");

	const std::array<std::uint8_t, 1> payload = { 1 };
	std::vector<std::uint8_t> output;
	const std::vector<wt::WtContainerSectionInput> duplicate = {
		{ 1, 0, wt::WtStorageCodec::None, { payload.data(), payload.size() } },
		{ 1, 0, wt::WtStorageCodec::None, { payload.data(), payload.size() } },
	};
	check(wt::wt_write_container(
		wt::kWtWorldMagic, 0, 0, duplicate, output
	) == wt::WtContainerStatus::DuplicateSection, "duplicate write accepted");
}

} // namespace

int main() {
	test_sha256();
	test_binary_io();
	test_container_round_trip();
	test_container_corruption();
	if (failure_count != 0) {
		std::fprintf(stderr, "M4_STORAGE_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M4_STORAGE_PASS sha_vectors=2 corruption_cases=8\n");
	return 0;
}
