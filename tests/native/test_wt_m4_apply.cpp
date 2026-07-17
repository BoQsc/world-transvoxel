#include "bake/wt_chunk_baker.h"
#include "editing/wt_chunk_edit_state.h"
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

class IntegerSource final : public wt::WtChunkSampleSource {
public:
	bool sample(
		const wt::WtGridPoint &point,
		wt::WtScalarSample &output
	) const noexcept override {
		output.density = static_cast<float>(point.x + point.y + point.z);
		output.material = 1;
		return true;
	}
};

wt::WtEditCommand sphere(
	std::uint8_t seed,
	std::uint32_t sequence,
	std::uint64_t revision,
	wt::WtEditOperation operation,
	std::int64_t x,
	std::uint64_t radius_q16,
	float density
) {
	wt::WtEditCommand command;
	command.command_id = id(seed);
	command.sequence = sequence;
	command.world_revision = revision;
	command.operation = operation;
	command.shape = wt::WtEditShape::Sphere;
	command.density_value = density;
	command.sphere = {
		x * wt::kWtEditCoordinateScale,
		0,
		0,
		radius_q16,
	};
	check(
		wt::wt_edit_sphere_bounds(command.sphere, command.bounds),
		"apply sphere bounds failed"
	);
	return command;
}

wt::WtEditCommand paint_box(
	std::uint8_t seed,
	std::uint32_t sequence,
	std::uint64_t revision
) {
	wt::WtEditCommand command;
	command.command_id = id(seed);
	command.sequence = sequence;
	command.world_revision = revision;
	command.operation = wt::WtEditOperation::PaintMaterial;
	command.shape = wt::WtEditShape::AxisAlignedBox;
	command.material = 9;
	command.box = {
		15 * wt::kWtEditCoordinateScale,
		0,
		0,
		17 * wt::kWtEditCoordinateScale,
		0,
		0,
	};
	check(
		wt::wt_edit_box_bounds(command.box, command.bounds),
		"apply box bounds failed"
	);
	return command;
}

wt::WtEditJournal make_journal() {
	wt::WtEditJournal journal(2, 4, 8192);
	journal.reset(100, 0);
	wt::WtEditTransaction first;
	first.source_revision = 100;
	first.transaction_id = id(1);
	first.base_revision = 0;
	first.committed_revision = 1;
	first.commands = {
		paint_box(40, 1, 1),
		sphere(
			20,
			0,
			1,
			wt::WtEditOperation::AddDensity,
			16,
			wt::kWtEditCoordinateScale,
			-2.0F
		),
	};
	std::vector<std::uint8_t> segment;
	check(
		journal.append(first, segment) == wt::WtEditJournalStatus::Ok,
		"apply journal first transaction failed"
	);

	wt::WtEditTransaction second;
	second.source_revision = 100;
	second.transaction_id = id(2);
	second.base_revision = 1;
	second.committed_revision = 2;
	second.commands = {
		sphere(
			80,
			1,
			2,
			wt::WtEditOperation::AddDensity,
			1000,
			wt::kWtEditCoordinateScale / 2,
			-1.0F
		),
		sphere(
			60,
			0,
			2,
			wt::WtEditOperation::SetDensity,
			0,
			wt::kWtEditCoordinateScale / 2,
			-5.0F
		),
	};
	check(
		journal.append(second, segment) == wt::WtEditJournalStatus::Ok,
		"apply journal second transaction failed"
	);
	return journal;
}

std::vector<wt::WtChunkPage> bake_pages() {
	const IntegerSource source;
	wt::WtChunkBaker baker(2);
	std::vector<wt::WtBakedChunkPage> baked;
	check(
		baker.bake(
			{ { 0, 0, 0, 0 }, { 1, 0, 0, 0 } },
			100,
			source,
			baked
		) == wt::WtChunkBakeStatus::Ok,
		"apply fixture bake failed"
	);
	std::vector<wt::WtChunkPage> pages;
	for (const wt::WtBakedChunkPage &item : baked) {
		wt::WtChunkPageView view;
		wt::WtChunkPage page;
		check(
			wt::wt_open_chunk_page(
				{ item.bytes.data(), item.bytes.size() },
				view
			) == wt::WtChunkPageStatus::Ok &&
				wt::wt_decode_chunk_page(view, page) ==
					wt::WtChunkPageStatus::Ok,
			"apply fixture page decode failed"
		);
		pages.push_back(std::move(page));
	}
	return pages;
}

wt::WtChunkPage bake_page(const wt::WtChunkKey &key) {
	const IntegerSource source;
	wt::WtChunkBaker baker(1);
	std::vector<wt::WtBakedChunkPage> baked;
	check(
		baker.bake({ key }, 100, source, baked) == wt::WtChunkBakeStatus::Ok &&
			baked.size() == 1,
		"apply single page fixture bake failed"
	);
	wt::WtChunkPageView view;
	wt::WtChunkPage page;
	check(
		wt::wt_open_chunk_page(
			{ baked[0].bytes.data(), baked[0].bytes.size() },
			view
		) == wt::WtChunkPageStatus::Ok &&
			wt::wt_decode_chunk_page(view, page) == wt::WtChunkPageStatus::Ok,
		"apply single page fixture decode failed"
	);
	return page;
}

std::size_t sample_index(
	const wt::WtChunkPage &page,
	const wt::WtGridPoint &point
) {
	const wt::WtChunkBounds bounds = wt::wt_chunk_bounds(page.metadata.key);
	const std::int64_t spacing =
		static_cast<std::int64_t>(page.metadata.cell_spacing);
	const std::int64_t x = (point.x - bounds.minimum.x) / spacing;
	const std::int64_t y = (point.y - bounds.minimum.y) / spacing;
	const std::int64_t z = (point.z - bounds.minimum.z) / spacing;
	return static_cast<std::size_t>(
		((z + 1) * 19 + (y + 1)) * 19 + (x + 1)
	);
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void test_replay_and_overlap(std::vector<std::uint8_t> &evidence) {
	std::vector<wt::WtChunkPage> pages = bake_pages();
	if (pages.size() != 2) return;
	wt::WtChunkEditState left;
	wt::WtChunkEditState right;
	check(
		left.initialize(std::move(pages[0]), 100, 0) ==
				wt::WtChunkEditStatus::Ok &&
			right.initialize(std::move(pages[1]), 100, 0) ==
				wt::WtChunkEditStatus::Ok,
		"chunk edit state initialization failed"
	);
	const wt::WtEditJournal journal = make_journal();
	check(
		journal.replay(left) == wt::WtEditJournalStatus::Ok &&
			journal.replay(right) == wt::WtEditJournalStatus::Ok,
		"chunk edit journal replay failed"
	);
	check(
		left.current_world_revision() == 2 &&
			right.current_world_revision() == 2 &&
			left.next_sequence() == 2 &&
			right.next_sequence() == 2,
		"chunk edit replay revision state mismatch"
	);
	check(
		left.changed_sample_count() == 11 &&
			right.changed_sample_count() == 10,
		"chunk edit changed-sample count mismatch"
	);

	const wt::WtScalarSample center =
		left.page().samples[sample_index(left.page(), { 16, 0, 0 })];
	check(
		center.density == 14.0F && center.material == 9,
		"overlap center edit result mismatch"
	);
	check(
		left.page().samples[sample_index(left.page(), { 0, 0, 0 })].density ==
			-5.0F,
		"set-density edit result mismatch"
	);
	for (std::int64_t z = -1; z <= 17; ++z) {
		for (std::int64_t y = -1; y <= 17; ++y) {
			for (std::int64_t x = 15; x <= 17; ++x) {
				const wt::WtGridPoint point = { x, y, z };
				const wt::WtScalarSample left_sample =
					left.page().samples[sample_index(left.page(), point)];
				const wt::WtScalarSample right_sample =
					right.page().samples[sample_index(right.page(), point)];
				check(
					left_sample.density == right_sample.density &&
						left_sample.material == right_sample.material,
					"neighbor page overlap diverged after replay"
				);
			}
		}
	}

	for (const wt::WtChunkEditState *state : { &left, &right }) {
		std::vector<std::uint8_t> bytes;
		check(
			wt::wt_write_chunk_page(state->page(), bytes) ==
				wt::WtChunkPageStatus::Ok,
			"edited chunk page write failed"
		);
		evidence.insert(evidence.end(), bytes.begin(), bytes.end());
		wt::WtChunkPageView view;
		wt::WtChunkPage decoded;
		std::vector<std::uint8_t> rewritten;
		check(
			wt::wt_open_chunk_page(
				{ bytes.data(), bytes.size() },
				view
			) == wt::WtChunkPageStatus::Ok &&
				wt::wt_decode_chunk_page(view, decoded) ==
					wt::WtChunkPageStatus::Ok &&
				wt::wt_write_chunk_page(decoded, rewritten) ==
					wt::WtChunkPageStatus::Ok &&
				rewritten == bytes,
			"edited chunk page save/load round trip failed"
		);
	}
}

void test_cross_lod_pointwise_edit_consistency(
	std::vector<std::uint8_t> &evidence
) {
	wt::WtChunkEditState fine;
	wt::WtChunkEditState coarse;
	check(
		fine.initialize(bake_page({ 0, 0, 0, 0 }), 100, 0) ==
				wt::WtChunkEditStatus::Ok &&
			coarse.initialize(bake_page({ 0, 0, 0, 3 }), 100, 0) ==
				wt::WtChunkEditStatus::Ok,
		"cross-LOD edit fixtures failed"
	);
	wt::WtEditCommand command = sphere(
		90,
		0,
		1,
		wt::WtEditOperation::AddDensity,
		0,
		wt::kWtEditCoordinateScale,
		-7.0F
	);
	check(
		fine.apply_command(command) == wt::WtChunkEditStatus::Ok &&
			coarse.apply_command(command) == wt::WtChunkEditStatus::Ok,
		"cross-LOD pointwise edit command failed"
	);
	check(
		fine.changed_sample_count() == 7 &&
			coarse.changed_sample_count() == 1,
		"cross-LOD edit changed the wrong sample footprints"
	);
	check(
		fine.page().samples[sample_index(fine.page(), { 0, 0, 0 })].density ==
				-7.0F &&
			coarse.page().samples[
				sample_index(coarse.page(), { 0, 0, 0 })
			].density ==
				-7.0F &&
			fine.page().samples[sample_index(fine.page(), { 8, 0, 0 })].density ==
				8.0F &&
			coarse.page().samples[
				sample_index(coarse.page(), { 8, 0, 0 })
			].density ==
				8.0F &&
			coarse.page().samples[
				sample_index(coarse.page(), { 16, 0, 0 })
			].density ==
				16.0F,
		"shared cross-LOD samples received different edit values"
	);
	std::vector<std::uint8_t> bytes;
	check(
		wt::wt_write_chunk_page(coarse.page(), bytes) ==
			wt::WtChunkPageStatus::InvalidInput,
		"stale coarse surface-shift data was serialized"
	);
	check(
		wt::wt_write_chunk_page(fine.page(), bytes) == wt::WtChunkPageStatus::Ok,
		"pointwise LOD0 edit page write failed"
	);
	evidence.insert(evidence.end(), bytes.begin(), bytes.end());
}

void test_sdf_sphere_edits(std::vector<std::uint8_t> &evidence) {
	wt::WtChunkEditState state;
	check(
		state.initialize(bake_page({ 0, 0, 0, 0 }), 100, 0) ==
			wt::WtChunkEditStatus::Ok,
		"SDF sphere edit fixture initialization failed"
	);
	wt::WtEditCommand carve = sphere(
		91,
		0,
		1,
		wt::WtEditOperation::SdfCarve,
		0,
		2 * wt::kWtEditCoordinateScale,
		1.0F
	);
	check(
		state.apply_command(carve) == wt::WtChunkEditStatus::Ok,
		"SDF carve sphere command failed"
	);
	check(
		state.page().samples[sample_index(state.page(), { 0, 0, 0 })].density ==
				2.0F &&
			state.page().samples[sample_index(state.page(), { -1, 0, 0 })].density ==
				1.0F &&
			state.page().samples[sample_index(state.page(), { 3, 0, 0 })].density ==
				3.0F,
		"SDF carve sphere density values mismatch"
	);
	wt::WtEditCommand construct = sphere(
		92,
		0,
		2,
		wt::WtEditOperation::SdfConstruct,
		0,
		wt::kWtEditCoordinateScale,
		2.0F
	);
	construct.material = 8;
	check(
		state.apply_command(construct) == wt::WtChunkEditStatus::Ok,
		"SDF construct sphere command failed"
	);
	check(
		state.page().samples[sample_index(state.page(), { 0, 0, 0 })].density ==
				-2.0F &&
			state.page().samples[sample_index(state.page(), { 0, 0, 0 })].material ==
				8 &&
			state.page().samples[sample_index(state.page(), { -1, 0, 0 })].density <
				0.0F &&
			state.page().samples[sample_index(state.page(), { -1, 0, 0 })].material ==
				8 &&
			state.page().samples[sample_index(state.page(), { 3, 0, 0 })].density ==
				3.0F &&
			state.page().samples[sample_index(state.page(), { 3, 0, 0 })].material ==
				1,
		"SDF construct sphere density/material values mismatch"
	);
	wt::WtChunkPage ownership_page = bake_page({ 0, 0, 0, 0 });
	ownership_page.samples[sample_index(ownership_page, { 0, 0, 0 })] = {
		-4.0F, 7
	};
	wt::WtChunkEditState ownership_state;
	check(
		ownership_state.initialize(ownership_page, 100, 1) ==
			wt::WtChunkEditStatus::Ok &&
		ownership_state.apply_command(construct) == wt::WtChunkEditStatus::Ok,
		"SDF construct material ownership fixture failed"
	);
	check(
		ownership_state.page().samples[
			sample_index(ownership_state.page(), { 0, 0, 0 })
		].density == -4.0F &&
		ownership_state.page().samples[
			sample_index(ownership_state.page(), { 0, 0, 0 })
		].material == 7,
		"SDF construct overwrote material of pre-existing denser solid"
	);
	wt::WtChunkPage support_page = bake_page({ 0, 0, 0, 0 });
	support_page.samples[sample_index(support_page, { 3, 0, 0 })].density = -4.0F;
	support_page.samples[sample_index(support_page, { 4, 0, 0 })].density = -5.0F;
	wt::WtChunkEditState support_state;
	check(
		support_state.initialize(support_page, 100, 0) ==
			wt::WtChunkEditStatus::Ok,
		"SDF support-band fixture initialization failed"
	);
	wt::WtEditCommand support_carve = sphere(
		93,
		0,
		1,
		wt::WtEditOperation::SdfCarve,
		0,
		2 * wt::kWtEditCoordinateScale,
		1.0F
	);
	check(
		support_state.apply_command(support_carve) == wt::WtChunkEditStatus::Ok,
		"SDF support-band carve command failed"
	);
	check(
		support_state.page().samples[
			sample_index(support_state.page(), { 3, 0, 0 })
		].density == -1.0F &&
			support_state.page().samples[
				sample_index(support_state.page(), { 4, 0, 0 })
			].density == -5.0F,
		"SDF support-band carve density values mismatch"
	);
	std::vector<std::uint8_t> bytes;
	check(
		wt::wt_write_chunk_page(state.page(), bytes) == wt::WtChunkPageStatus::Ok,
		"SDF sphere edited page write failed"
	);
	evidence.insert(evidence.end(), bytes.begin(), bytes.end());
	check(
		wt::wt_write_chunk_page(support_state.page(), bytes) ==
			wt::WtChunkPageStatus::Ok,
		"SDF support-band page write failed"
	);
	evidence.insert(evidence.end(), bytes.begin(), bytes.end());
}

void test_material_volume_placement() {
	wt::WtChunkPage page = bake_page({ 0, 0, 0, 0 });
	const std::size_t solid_index = sample_index(page, { 0, 0, 0 });
	const std::size_t air_index = sample_index(page, { 1, 0, 0 });
	page.samples[solid_index] = { -2.0F, 3 };
	page.samples[air_index] = { 2.0F, 1 };
	wt::WtChunkEditState state;
	check(
		state.initialize(page, 100, 0) == wt::WtChunkEditStatus::Ok,
		"material volume fixture initialization failed"
	);
	wt::WtEditCommand place = sphere(
		94,
		0,
		1,
		wt::WtEditOperation::PlaceMaterialVolume,
		0,
		2 * wt::kWtEditCoordinateScale,
		0.0F
	);
	place.material = 9;
	check(
		state.apply_command(place) == wt::WtChunkEditStatus::Ok,
		"material volume placement failed"
	);
	check(
		state.page().samples[solid_index].density == -2.0F &&
		state.page().samples[solid_index].material == 3 &&
		state.page().samples[air_index].density == 2.0F &&
		state.page().samples[air_index].material == 9,
		"material volume placement repainted solid or missed air"
	);
}

void test_failures() {
	std::vector<wt::WtChunkPage> pages = bake_pages();
	if (pages.empty()) return;
	wt::WtChunkEditState state;
	wt::WtEditCommand valid = sphere(
		20,
		0,
		1,
		wt::WtEditOperation::AddDensity,
		0,
		wt::kWtEditCoordinateScale / 2,
		-1.0F
	);
	check(
		state.apply_command(valid) == wt::WtChunkEditStatus::NotInitialized,
		"uninitialized edit state accepted a command"
	);
	wt::WtChunkPage invalid = pages[0];
	invalid.samples.pop_back();
	check(
		state.initialize(std::move(invalid), 100, 0) ==
			wt::WtChunkEditStatus::InvalidPage,
		"invalid page initialized"
	);
	check(
		state.initialize(pages[0], 101, 0) ==
			wt::WtChunkEditStatus::SourceRevisionMismatch,
		"source revision mismatch initialized"
	);
	check(
		state.initialize(pages[0], 100, 0) == wt::WtChunkEditStatus::Ok,
		"failure fixture initialization failed"
	);

	wt::WtEditCommand gap = valid;
	gap.world_revision = 2;
	check(
		state.apply_command(gap) ==
			wt::WtChunkEditStatus::WorldRevisionMismatch,
		"world revision gap was accepted"
	);
	wt::WtEditCommand sequence = valid;
	sequence.sequence = 1;
	check(
		state.apply_command(sequence) ==
			wt::WtChunkEditStatus::SequenceMismatch,
		"first command with nonzero sequence was accepted"
	);
	wt::WtEditCommand invalid_command = valid;
	invalid_command.bounds.maximum.x += 1;
	check(
		state.apply_command(invalid_command) ==
			wt::WtChunkEditStatus::InvalidCommand,
		"invalid command bounds were accepted"
	);

	wt::WtChunkPage overflow_page = pages[0];
	const std::size_t center_index =
		sample_index(overflow_page, { 0, 0, 0 });
	overflow_page.samples[center_index].density =
		std::numeric_limits<float>::max();
	check(
		state.initialize(std::move(overflow_page), 100, 0) ==
			wt::WtChunkEditStatus::Ok,
		"overflow fixture initialization failed"
	);
	wt::WtEditCommand overflow = valid;
	overflow.density_value = std::numeric_limits<float>::max();
	check(
		state.apply_command(overflow) ==
				wt::WtChunkEditStatus::NonFiniteResult &&
			state.current_world_revision() == 0 &&
			state.page().samples[center_index].density ==
				std::numeric_limits<float>::max(),
		"non-finite edit result mutated page or revision"
	);
}

} // namespace

int main() {
	std::vector<std::uint8_t> evidence;
	test_replay_and_overlap(evidence);
	test_cross_lod_pointwise_edit_consistency(evidence);
	test_sdf_sphere_edits(evidence);
	test_material_volume_placement();
	test_failures();
	if (failure_count != 0) {
		std::fprintf(stderr, "M4_APPLY_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M4_APPLY_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"M4_APPLY_PASS pages=5 overlap_samples=1083 cross_lod_pointwise=1 sdf_sphere_edits=3 failure_cases=7\n"
	);
	return 0;
}
