#include "editing/wt_edit_spatial_index.h"
#include "storage/wt_binary_io.h"
#include "storage/wt_hash256.h"

#include <algorithm>
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

std::vector<wt::WtChunkKey> make_keys() {
	std::vector<wt::WtChunkKey> keys;
	for (std::uint8_t lod = 0; lod <= 2; ++lod) {
		for (std::int32_t z = -1; z <= 0; ++z) {
			for (std::int32_t y = -1; y <= 0; ++y) {
				for (std::int32_t x = -1; x <= 0; ++x) {
					keys.push_back({ x, y, z, lod });
				}
			}
		}
	}
	keys.push_back({ -2, 0, 0, 0 });
	keys.push_back({ 1, 0, 0, 0 });
	keys.push_back({ 10, 10, 10, 0 });
	std::reverse(keys.begin(), keys.end());
	return keys;
}

wt::WtEditCommand sphere_command(
	std::uint32_t sequence,
	std::int64_t center
) {
	wt::WtEditCommand command;
	command.command_id = id(static_cast<std::uint8_t>(20 + sequence * 20));
	command.sequence = sequence;
	command.world_revision = 8;
	command.operation = wt::WtEditOperation::AddDensity;
	command.shape = wt::WtEditShape::Sphere;
	command.density_value = -1.0F;
	command.sphere = {
		center * wt::kWtEditCoordinateScale,
		center * wt::kWtEditCoordinateScale,
		center * wt::kWtEditCoordinateScale,
		static_cast<std::uint64_t>(wt::kWtEditCoordinateScale / 4),
	};
	check(
		wt::wt_edit_sphere_bounds(command.sphere, command.bounds),
		"spatial sphere bounds construction failed"
	);
	return command;
}

void append_keys(
	const std::vector<wt::WtChunkKey> &keys,
	wt::WtBinaryWriter &writer
) {
	for (const wt::WtChunkKey &key : keys) {
		check(writer.write_i32(key.x) == wt::WtBinaryStatus::Ok,
			"spatial hash write x failed");
		check(writer.write_i32(key.y) == wt::WtBinaryStatus::Ok,
			"spatial hash write y failed");
		check(writer.write_i32(key.z) == wt::WtBinaryStatus::Ok,
			"spatial hash write z failed");
		check(writer.write_u8(key.lod) == wt::WtBinaryStatus::Ok,
			"spatial hash write lod failed");
	}
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void test_queries(wt::WtBinaryWriter &evidence) {
	const std::vector<wt::WtChunkKey> keys = make_keys();
	wt::WtEditSpatialIndex index(keys.size(), 256, keys.size());
	check(
		index.rebuild(keys) == wt::WtEditSpatialStatus::Ok &&
			index.size() == keys.size(),
		"spatial index rebuild failed"
	);
	check(
		index.key_capacity() == keys.size() &&
			index.candidate_capacity() == 256 &&
			index.result_capacity() == keys.size(),
		"spatial index capacities mismatch"
	);

	std::vector<wt::WtChunkKey> output;
	check(
		index.query_bounds({ { 0, 0, 0 }, { 0, 0, 0 } }, output) ==
			wt::WtEditSpatialStatus::Ok,
		"origin spatial query failed"
	);
	check(output.size() == 24, "origin query missed padded LOD dependents");
	for (std::size_t index_value = 1; index_value < output.size(); ++index_value) {
		check(output[index_value - 1] < output[index_value],
			"origin query output is not canonical");
	}
	check(
		std::find(output.begin(), output.end(), wt::WtChunkKey{ -1, -1, -1, 2 }) !=
				output.end() &&
			std::find(output.begin(), output.end(), wt::WtChunkKey{ 0, 0, 0, 0 }) !=
				output.end() &&
			std::find(output.begin(), output.end(), wt::WtChunkKey{ 10, 10, 10, 0 }) ==
				output.end(),
		"origin query membership mismatch"
	);
	append_keys(output, evidence);

	check(
		index.query_bounds({ { 8, 8, 8 }, { 8, 8, 8 } }, output) ==
			wt::WtEditSpatialStatus::Ok &&
			output == std::vector<wt::WtChunkKey>({
				{ 0, 0, 0, 0 },
				{ 0, 0, 0, 1 },
				{ 0, 0, 0, 2 },
			}),
		"interior query affected unexpected chunks"
	);
	append_keys(output, evidence);

	check(
		index.query_bounds({ { -16, 8, 8 }, { -16, 8, 8 } }, output) ==
			wt::WtEditSpatialStatus::Ok &&
			output == std::vector<wt::WtChunkKey>({
				{ -2, 0, 0, 0 },
				{ -1, 0, 0, 0 },
				{ -1, 0, 0, 1 },
				{ -1, 0, 0, 2 },
			}),
		"negative boundary query missed padded neighbors"
	);
	append_keys(output, evidence);

	wt::WtEditTransaction transaction;
	transaction.transaction_id = id(1);
	transaction.base_revision = 7;
	transaction.committed_revision = 8;
	transaction.commands = {
		sphere_command(1, 160),
		sphere_command(0, 8),
	};
	check(
		index.query_transaction(transaction, output) ==
			wt::WtEditSpatialStatus::Ok &&
			output == std::vector<wt::WtChunkKey>({
				{ 0, 0, 0, 0 },
				{ 10, 10, 10, 0 },
				{ 0, 0, 0, 1 },
				{ 0, 0, 0, 2 },
			}),
		"transaction query merged unrelated space or missed keys"
	);
	append_keys(output, evidence);
}

void test_failures() {
	const std::vector<wt::WtChunkKey> keys = make_keys();
	wt::WtEditSpatialIndex small_index(1, 32, 1);
	check(
		small_index.rebuild({ { 0, 0, 0, 0 } }) ==
			wt::WtEditSpatialStatus::Ok,
		"small spatial fixture rebuild failed"
	);
	check(
		small_index.rebuild({ { 0, 0, 0, 0 }, { 1, 0, 0, 0 } }) ==
			wt::WtEditSpatialStatus::KeyCapacityExceeded &&
			small_index.size() == 1,
		"key capacity failure replaced the existing index"
	);
	check(
		small_index.rebuild({ { 0, 0, 0, 0 }, { 0, 0, 0, 0 } }) ==
			wt::WtEditSpatialStatus::KeyCapacityExceeded,
		"capacity was not checked before duplicate rebuild"
	);

	wt::WtEditSpatialIndex index(keys.size(), 256, keys.size());
	check(index.rebuild(keys) == wt::WtEditSpatialStatus::Ok,
		"failure fixture rebuild failed");
	check(
		index.rebuild({ { 0, 0, 0, 21 } }) ==
			wt::WtEditSpatialStatus::InvalidInput &&
			index.size() == keys.size(),
		"invalid-key rebuild replaced the existing index"
	);
	check(
		index.rebuild({ { 0, 0, 0, 0 }, { 0, 0, 0, 0 } }) ==
			wt::WtEditSpatialStatus::DuplicateKey &&
			index.size() == keys.size(),
		"duplicate-key rebuild replaced the existing index"
	);

	std::vector<wt::WtChunkKey> output = { { 99, 99, 99, 0 } };
	check(
		index.query_bounds({ { 1, 0, 0 }, { 0, 0, 0 } }, output) ==
			wt::WtEditSpatialStatus::InvalidInput &&
			output.empty(),
		"inverted query bounds were accepted"
	);

	wt::WtEditSpatialIndex candidate_limited(keys.size(), 10, keys.size());
	check(candidate_limited.rebuild(keys) == wt::WtEditSpatialStatus::Ok,
		"candidate-limit fixture rebuild failed");
	check(
		candidate_limited.query_bounds(
			{ { 0, 0, 0 }, { 0, 0, 0 } },
			output
		) == wt::WtEditSpatialStatus::CandidateCapacityExceeded &&
			output.empty(),
		"candidate capacity overflow was accepted"
	);
	check(
		candidate_limited.query_bounds(
			{
				{
					std::numeric_limits<std::int64_t>::min(),
					std::numeric_limits<std::int64_t>::min(),
					std::numeric_limits<std::int64_t>::min(),
				},
				{
					std::numeric_limits<std::int64_t>::max(),
					std::numeric_limits<std::int64_t>::max(),
					std::numeric_limits<std::int64_t>::max(),
				},
			},
			output
		) == wt::WtEditSpatialStatus::CandidateCapacityExceeded &&
			output.empty(),
		"extreme query bounds overflowed or were accepted"
	);

	wt::WtEditSpatialIndex result_limited(keys.size(), 256, 4);
	check(result_limited.rebuild(keys) == wt::WtEditSpatialStatus::Ok,
		"result-limit fixture rebuild failed");
	check(
		result_limited.query_bounds(
			{ { 0, 0, 0 }, { 0, 0, 0 } },
			output
		) == wt::WtEditSpatialStatus::ResultCapacityExceeded &&
			output.empty(),
		"result capacity overflow was accepted"
	);

	wt::WtEditTransaction transaction;
	check(
		index.query_transaction(transaction, output) ==
			wt::WtEditSpatialStatus::InvalidInput &&
			output.empty(),
		"empty transaction query was accepted"
	);
	transaction.transaction_id = id(1);
	transaction.base_revision = 7;
	transaction.committed_revision = 8;
	transaction.commands = { sphere_command(0, 8), sphere_command(0, 9) };
	check(
		index.query_transaction(transaction, output) ==
			wt::WtEditSpatialStatus::InvalidInput &&
			output.empty(),
		"duplicate transaction sequence was accepted"
	);
}

} // namespace

int main() {
	wt::WtBinaryWriter evidence(35 * 13);
	test_queries(evidence);
	test_failures();
	if (failure_count != 0) {
		std::fprintf(stderr, "M4_SPATIAL_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M4_SPATIAL_HASH ");
	print_hash(wt::wt_sha256(evidence.bytes().data(), evidence.bytes().size()));
	std::printf("M4_SPATIAL_PASS indexed_keys=27 failure_cases=10\n");
	return 0;
}
