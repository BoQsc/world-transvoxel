#include "bake/wt_chunk_baker.h"
#include "storage/wt_chunk_page.h"
#include "storage/wt_hash256.h"

#include <array>
#include <cmath>
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

class LinearSource final : public wt::WtChunkSampleSource {
public:
	bool sample(
		const wt::WtGridPoint &point,
		wt::WtScalarSample &output
	) const noexcept override {
		output.density =
			static_cast<float>(point.x) * 0.25F -
			static_cast<float>(point.y) * 0.5F +
			static_cast<float>(point.z) * 0.75F -
			3.125F;
		const std::uint64_t mixed =
			static_cast<std::uint64_t>(point.x) * 73856093ULL ^
			static_cast<std::uint64_t>(point.y) * 19349663ULL ^
			static_cast<std::uint64_t>(point.z) * 83492791ULL;
		output.material = static_cast<std::uint16_t>(mixed);
		return true;
	}
};

class FailingSource final : public wt::WtChunkSampleSource {
public:
	bool sample(
		const wt::WtGridPoint &,
		wt::WtScalarSample &
	) const noexcept override {
		return false;
	}
};

class NonFiniteSource final : public wt::WtChunkSampleSource {
public:
	bool sample(
		const wt::WtGridPoint &,
		wt::WtScalarSample &output
	) const noexcept override {
		output.density = std::numeric_limits<float>::infinity();
		return true;
	}
};

wt::WtScalarSample expected_sample(const wt::WtGridPoint &point) {
	LinearSource source;
	wt::WtScalarSample output;
	source.sample(point, output);
	return output;
}

std::size_t sample_index(int x, int y, int z) {
	return static_cast<std::size_t>(
		((z + 1) * 19 + (y + 1)) * 19 + (x + 1)
	);
}

bool same_sample(
	const wt::WtScalarSample &left,
	const wt::WtScalarSample &right
) {
	return left.density == right.density && left.material == right.material;
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void validate_page(
	const wt::WtBakedChunkPage &baked,
	std::uint64_t source_revision
) {
	check(
		baked.content_hash ==
			wt::wt_sha256(baked.bytes.data(), baked.bytes.size()),
		"baked content hash mismatch"
	);
	wt::WtChunkPageView view;
	check(
		wt::wt_open_chunk_page(
			{ baked.bytes.data(), baked.bytes.size() },
			view
		) == wt::WtChunkPageStatus::Ok,
		"chunk page open failed"
	);
	check(view.metadata.key == baked.key, "chunk page key mismatch");
	check(
		view.metadata.source_revision == source_revision,
		"chunk page source revision mismatch"
	);
	check(
		view.metadata.sample_minimum == -1 &&
			view.metadata.sample_maximum == 17 &&
			view.metadata.dimension_x == 19 &&
			view.metadata.dimension_y == 19 &&
			view.metadata.dimension_z == 19 &&
			view.metadata.sample_count == wt::kWtChunkPageSampleCount,
		"chunk page sampling metadata mismatch"
	);
	check(
		view.encoded_samples.data >= baked.bytes.data() &&
			view.encoded_samples.data <
				baked.bytes.data() + baked.bytes.size(),
		"chunk page payload was copied instead of viewed"
	);

	wt::WtChunkPage decoded;
	check(
		wt::wt_decode_chunk_page(view, decoded) == wt::WtChunkPageStatus::Ok,
		"chunk page decode failed"
	);
	check(
		decoded.samples.size() == wt::kWtChunkPageSampleCount,
		"decoded sample count mismatch"
	);
	if (decoded.samples.size() != wt::kWtChunkPageSampleCount) {
		return;
	}

	const wt::WtChunkBounds bounds = wt::wt_chunk_bounds(baked.key);
	const std::int64_t spacing = wt::wt_lod_cell_size(baked.key.lod);
	const std::array<std::array<int, 3>, 3> coordinates = {{
		{{ -1, -1, -1 }},
		{{ 0, 0, 0 }},
		{{ 17, 17, 17 }},
	}};
	for (const std::array<int, 3> &coordinate : coordinates) {
		const wt::WtGridPoint point = {
			bounds.minimum.x + coordinate[0] * spacing,
			bounds.minimum.y + coordinate[1] * spacing,
			bounds.minimum.z + coordinate[2] * spacing,
		};
		check(
			same_sample(
				decoded.samples[sample_index(
					coordinate[0],
					coordinate[1],
					coordinate[2]
				)],
				expected_sample(point)
			),
			"decoded sample value mismatch"
		);
	}

	std::vector<std::uint8_t> rewritten;
	check(
		wt::wt_write_chunk_page(decoded, rewritten) ==
			wt::WtChunkPageStatus::Ok,
		"decoded page rewrite failed"
	);
	check(rewritten == baked.bytes, "chunk page round trip changed bytes");
}

void test_schema_rejection(const wt::WtBakedChunkPage &valid) {
	wt::WtChunkPageView valid_view;
	check(
		wt::wt_open_chunk_page(
			{ valid.bytes.data(), valid.bytes.size() },
			valid_view
		) == wt::WtChunkPageStatus::Ok,
		"schema rejection fixture failed to open"
	);
	const wt::WtContainerSection *header =
		valid_view.container.find_section(wt::kWtChunkHeaderSection);
	const wt::WtContainerSection *data =
		valid_view.container.find_section(wt::kWtChunkDataSection);
	if (header == nullptr || data == nullptr) {
		check(false, "schema rejection fixture sections missing");
		return;
	}

	const std::array<std::uint8_t, 1> extra_payload = { 0 };
	std::vector<std::uint8_t> bytes;
	wt::WtChunkPageView rejected;
	std::vector<wt::WtContainerSectionInput> sections = {
		{ wt::kWtChunkHeaderSection, 1, wt::WtStorageCodec::None, header->payload },
		{ wt::kWtChunkDataSection, 0, wt::WtStorageCodec::None, data->payload },
	};
	check(
		wt::wt_write_container(
			wt::kWtChunkMagic,
			0,
			99,
			sections,
			bytes
		) == wt::WtContainerStatus::Ok &&
			wt::wt_open_chunk_page(
				{ bytes.data(), bytes.size() },
				rejected
			) == wt::WtChunkPageStatus::InvalidMetadata,
		"nonzero chunk section flags were accepted"
	);

	sections[0].flags = 0;
	check(
		wt::wt_write_container(
			wt::kWtChunkMagic,
			0,
			100,
			sections,
			bytes
		) == wt::WtContainerStatus::Ok &&
			wt::wt_open_chunk_page(
				{ bytes.data(), bytes.size() },
				rejected
			) == wt::WtChunkPageStatus::InvalidMetadata,
		"chunk source revision disagreement was accepted"
	);

	sections.pop_back();
	check(
		wt::wt_write_container(
			wt::kWtChunkMagic,
			0,
			99,
			sections,
			bytes
		) == wt::WtContainerStatus::Ok &&
			wt::wt_open_chunk_page(
				{ bytes.data(), bytes.size() },
				rejected
			) == wt::WtChunkPageStatus::InvalidMetadata,
		"missing chunk section was accepted"
	);

	sections.push_back(
		{ wt::kWtChunkDataSection, 0, wt::WtStorageCodec::None, data->payload }
	);
	sections.push_back(
		{ wt::wt_fourcc('E', 'X', 'T', 'R'), 0, wt::WtStorageCodec::None,
			{ extra_payload.data(), extra_payload.size() } }
	);
	check(
		wt::wt_write_container(
			wt::kWtChunkMagic,
			0,
			99,
			sections,
			bytes
		) == wt::WtContainerStatus::Ok &&
			wt::wt_open_chunk_page(
				{ bytes.data(), bytes.size() },
				rejected
			) == wt::WtChunkPageStatus::InvalidMetadata,
		"extra chunk section was accepted"
	);
}

void test_deterministic_bake(wt::WtHash256 &aggregate_hash) {
	const LinearSource source;
	const std::vector<wt::WtChunkKey> keys = {
		{ -1, 0, 0, 1 },
		{ 1, 0, 0, 0 },
		{ 0, 0, 0, 0 },
	};
	std::vector<wt::WtChunkKey> reversed(keys.rbegin(), keys.rend());
	wt::WtChunkBaker baker(keys.size());
	std::vector<wt::WtBakedChunkPage> first;
	std::vector<wt::WtBakedChunkPage> second;
	check(
		baker.bake(keys, 99, source, first) == wt::WtChunkBakeStatus::Ok,
		"first deterministic bake failed"
	);
	check(
		baker.bake(reversed, 99, source, second) ==
			wt::WtChunkBakeStatus::Ok,
		"second deterministic bake failed"
	);
	check(first.size() == 3 && second.size() == 3, "baked page count mismatch");
	if (first.size() != 3 || second.size() != 3) {
		return;
	}
	check(first[0].key == wt::WtChunkKey{ 0, 0, 0, 0 }, "page order 0 mismatch");
	check(first[1].key == wt::WtChunkKey{ 1, 0, 0, 0 }, "page order 1 mismatch");
	check(first[2].key == wt::WtChunkKey{ -1, 0, 0, 1 }, "page order 2 mismatch");

	std::vector<std::uint8_t> hashes;
	hashes.reserve(first.size() * first[0].content_hash.size());
	for (std::size_t index = 0; index < first.size(); ++index) {
		check(first[index].key == second[index].key, "repeat page key mismatch");
		check(
			first[index].content_hash == second[index].content_hash,
			"repeat page hash mismatch"
		);
		check(first[index].bytes == second[index].bytes, "repeat page bytes mismatch");
		validate_page(first[index], 99);
		hashes.insert(
			hashes.end(),
			first[index].content_hash.begin(),
			first[index].content_hash.end()
		);
	}
	aggregate_hash = wt::wt_sha256(hashes.data(), hashes.size());
	test_schema_rejection(first[0]);

	std::vector<std::uint8_t> corrupted = first[0].bytes;
	corrupted.back() ^= 0x80U;
	wt::WtChunkPageView view;
	check(
		wt::wt_open_chunk_page(
			{ corrupted.data(), corrupted.size() },
			view
		) == wt::WtChunkPageStatus::ContainerFailure,
		"corrupted chunk page was accepted"
	);
}

void test_failure_contracts() {
	const LinearSource source;
	const FailingSource failing_source;
	const NonFiniteSource nonfinite_source;
	std::vector<wt::WtBakedChunkPage> output;

	wt::WtChunkBaker capacity_baker(1);
	check(
		capacity_baker.bake(
			{ { 0, 0, 0, 0 }, { 1, 0, 0, 0 } },
			1,
			source,
			output
		) == wt::WtChunkBakeStatus::PageCapacityExceeded &&
			output.empty(),
		"page capacity failure contract mismatch"
	);

	wt::WtChunkBaker baker(2);
	check(
		baker.bake(
			{ { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },
			1,
			source,
			output
		) == wt::WtChunkBakeStatus::DuplicateKey &&
			output.empty(),
		"duplicate key failure contract mismatch"
	);
	check(
		baker.bake(
			{ { 0, 0, 0, 21 } },
			1,
			source,
			output
		) == wt::WtChunkBakeStatus::InvalidInput &&
			output.empty(),
		"invalid key failure contract mismatch"
	);
	check(
		baker.bake(
			{ { 0, 0, 0, 0 } },
			1,
			failing_source,
			output
		) == wt::WtChunkBakeStatus::SampleSourceFailure &&
			output.empty(),
		"sample source failure contract mismatch"
	);
	check(
		baker.bake(
			{ { 0, 0, 0, 0 } },
			1,
			nonfinite_source,
			output
		) == wt::WtChunkBakeStatus::SampleSourceFailure &&
			output.empty(),
		"non-finite source failure contract mismatch"
	);

	wt::WtChunkPage invalid_page;
	invalid_page.samples.resize(wt::kWtChunkPageSampleCount);
	invalid_page.metadata.dimension_x = 18;
	std::vector<std::uint8_t> bytes;
	check(
		wt::wt_write_chunk_page(invalid_page, bytes) ==
			wt::WtChunkPageStatus::InvalidInput &&
			bytes.empty(),
		"invalid page metadata was accepted"
	);
	invalid_page.metadata.dimension_x = 19;
	invalid_page.samples[0].density =
		std::numeric_limits<float>::quiet_NaN();
	check(
		wt::wt_write_chunk_page(invalid_page, bytes) ==
			wt::WtChunkPageStatus::InvalidSample &&
			bytes.empty(),
		"non-finite page sample was accepted"
	);
}

} // namespace

int main() {
	wt::WtHash256 aggregate_hash{};
	test_deterministic_bake(aggregate_hash);
	test_failure_contracts();
	if (failure_count != 0) {
		std::fprintf(stderr, "M4_BAKE_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M4_BAKE_HASH ");
	print_hash(aggregate_hash);
	std::printf(
		"M4_BAKE_PASS pages=3 samples_per_page=%zu failure_cases=11\n",
		wt::kWtChunkPageSampleCount
	);
	return 0;
}
