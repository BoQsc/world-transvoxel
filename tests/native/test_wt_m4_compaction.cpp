#include "bake/wt_snapshot_compactor.h"
#include "editing/wt_chunk_edit_state.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
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

wt::WtHash256 hash_text(const char *text) {
	const std::string value(text);
	return wt::wt_sha256(
		reinterpret_cast<const std::uint8_t *>(value.data()),
		value.size()
	);
}

wt::WtId128 id(std::uint8_t seed) {
	wt::WtId128 value{};
	for (std::size_t index = 0; index < value.size(); ++index) {
		value[index] = static_cast<std::uint8_t>(seed + index);
	}
	return value;
}

class Source final : public wt::WtChunkSampleSource {
public:
	bool sample(
		const wt::WtGridPoint &point,
		wt::WtScalarSample &output
	) const noexcept override {
		output.density = static_cast<float>(point.x - point.y + point.z);
		output.material = 2;
		return true;
	}
};

wt::WtDependencyEntry dependency(
	wt::WtDependencyKind kind,
	const char *label,
	const char *version,
	const char *identity
) {
	return { kind, label, version, hash_text(identity) };
}

std::vector<wt::WtDependencyEntry> dependencies() {
	return {
		dependency(wt::WtDependencyKind::SourceAsset, "terrain", "", "terrain"),
		dependency(wt::WtDependencyKind::Generator, "baker", "1", "baker"),
		dependency(
			wt::WtDependencyKind::Configuration,
			"config",
			"1",
			"config"
		),
		dependency(wt::WtDependencyKind::Backend, "mit", "1", "mit"),
		dependency(wt::WtDependencyKind::Godot, "godot", "4.6.3", "godot"),
		dependency(
			wt::WtDependencyKind::GodotCpp,
			"godot-cpp",
			"e83f",
			"godot-cpp"
		),
		dependency(wt::WtDependencyKind::Toolchain, "zig", "0.16.0", "zig"),
	};
}

std::vector<wt::WtBakedChunkPage> bake_pages() {
	const Source source;
	wt::WtChunkBaker baker(2);
	std::vector<wt::WtBakedChunkPage> pages;
	check(
		baker.bake(
			{ { 1, 0, 0, 0 }, { 0, 0, 0, 0 } },
			100,
			source,
			pages
		) == wt::WtChunkBakeStatus::Ok,
		"compaction fixture bake failed"
	);
	return pages;
}

std::vector<std::uint8_t> write_world(
	const std::vector<wt::WtBakedChunkPage> &pages
) {
	wt::WtWorldManifest manifest;
	manifest.source_revision = 100;
	manifest.world_revision = 0;
	manifest.configuration_hash = hash_text("config");
	manifest.dependencies = dependencies();
	for (const wt::WtBakedChunkPage &page : pages) {
		manifest.pages.push_back({
			page.key,
			page.bytes.size(),
			page.content_hash,
		});
	}
	std::vector<std::uint8_t> bytes;
	check(
		wt::wt_write_world_manifest(manifest, bytes) ==
			wt::WtWorldManifestStatus::Ok,
		"compaction fixture world write failed"
	);
	return bytes;
}

wt::WtEditCommand sphere(
	std::uint8_t seed,
	std::uint32_t sequence,
	std::uint64_t revision,
	std::int64_t center,
	float density
) {
	wt::WtEditCommand command;
	command.command_id = id(seed);
	command.sequence = sequence;
	command.world_revision = revision;
	command.operation = wt::WtEditOperation::AddDensity;
	command.shape = wt::WtEditShape::Sphere;
	command.density_value = density;
	command.sphere = {
		center * wt::kWtEditCoordinateScale,
		0,
		0,
		static_cast<std::uint64_t>(wt::kWtEditCoordinateScale),
	};
	check(
		wt::wt_edit_sphere_bounds(command.sphere, command.bounds),
		"compaction sphere bounds failed"
	);
	return command;
}

wt::WtEditJournal make_journal(
	std::uint64_t source_revision = 100,
	std::uint64_t initial_revision = 0
) {
	wt::WtEditJournal journal(2, 3, 8192);
	journal.reset(source_revision, initial_revision);
	std::vector<std::uint8_t> segment;
	for (std::uint64_t index = 0; index < 2; ++index) {
		wt::WtEditTransaction transaction;
		transaction.source_revision = source_revision;
		transaction.transaction_id = id(static_cast<std::uint8_t>(1 + index));
		transaction.base_revision = initial_revision + index;
		transaction.committed_revision = initial_revision + index + 1;
		transaction.commands = {
			sphere(
				static_cast<std::uint8_t>(20 + index * 20),
				0,
				transaction.committed_revision,
				static_cast<std::int64_t>(index * 16),
				index == 0 ? -2.0F : 3.0F
			),
		};
		check(
			journal.append(transaction, segment) ==
				wt::WtEditJournalStatus::Ok,
			"compaction fixture journal append failed"
		);
	}
	return journal;
}

wt::WtChunkPage decode_page(const wt::WtBakedChunkPage &baked) {
	wt::WtChunkPageView view;
	wt::WtChunkPage page;
	check(
		wt::wt_open_chunk_page(
			{ baked.bytes.data(), baked.bytes.size() },
			view
		) == wt::WtChunkPageStatus::Ok &&
			wt::wt_decode_chunk_page(view, page) ==
				wt::WtChunkPageStatus::Ok,
		"compacted page decode failed"
	);
	return page;
}

void check_sample_equivalence(
	const std::vector<wt::WtBakedChunkPage> &source_pages,
	const wt::WtEditJournal &journal,
	const wt::WtCompactedSnapshot &compacted
) {
	for (std::size_t index = 0; index < source_pages.size(); ++index) {
		wt::WtChunkPage source = decode_page(source_pages[index]);
		wt::WtChunkEditState state;
		check(
			state.initialize(std::move(source), 100, 0) ==
					wt::WtChunkEditStatus::Ok &&
				journal.replay(state) == wt::WtEditJournalStatus::Ok,
			"direct replay for compaction comparison failed"
		);
		const wt::WtChunkPage snapshot = decode_page(compacted.pages[index]);
		check(
			snapshot.metadata.source_revision == 101 &&
				snapshot.metadata.key == state.page().metadata.key &&
				snapshot.samples.size() == state.page().samples.size(),
			"compacted page metadata mismatch"
		);
		if (snapshot.samples.size() != state.page().samples.size()) continue;
		for (std::size_t sample = 0; sample < snapshot.samples.size(); ++sample) {
			check(
				snapshot.samples[sample].density ==
						state.page().samples[sample].density &&
					snapshot.samples[sample].material ==
						state.page().samples[sample].material,
				"compacted samples differ from direct replay"
			);
		}
	}
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void test_compaction(wt::WtCompactedSnapshot &compacted) {
	std::vector<wt::WtBakedChunkPage> pages = bake_pages();
	const std::vector<std::uint8_t> world = write_world(pages);
	const wt::WtEditJournal journal = make_journal();
	check(
		wt::wt_compact_snapshot(
			{ world.data(), world.size() },
			pages,
			journal,
			101,
			2,
			compacted
		) == wt::WtSnapshotCompactionStatus::Ok,
		"snapshot compaction failed"
	);
	std::reverse(pages.begin(), pages.end());
	wt::WtCompactedSnapshot repeated;
	check(
		wt::wt_compact_snapshot(
			{ world.data(), world.size() },
			pages,
			journal,
			101,
			2,
			repeated
		) == wt::WtSnapshotCompactionStatus::Ok &&
			repeated.world_bytes == compacted.world_bytes &&
			repeated.pages.size() == compacted.pages.size(),
		"snapshot compaction depends on page input order"
	);
	if (repeated.pages.size() == compacted.pages.size()) {
		for (std::size_t index = 0; index < compacted.pages.size(); ++index) {
			check(
				repeated.pages[index].bytes == compacted.pages[index].bytes &&
					repeated.pages[index].content_hash ==
						compacted.pages[index].content_hash,
				"repeated compacted page mismatch"
			);
		}
	}

	wt::WtWorldManifestView view;
	check(
		wt::wt_open_world_manifest(
			{ compacted.world_bytes.data(), compacted.world_bytes.size() },
			view
		) == wt::WtWorldManifestStatus::Ok &&
			view.source_revision == 101 &&
			view.world_revision == 2 &&
			view.pages.size() == 2 &&
			view.dependencies.size() == 9,
		"compacted world metadata mismatch"
	);
	for (const wt::WtBakedChunkPage &page : compacted.pages) {
		check(
			wt::wt_validate_world_page(
				view,
				page.key,
				{ page.bytes.data(), page.bytes.size() }
			) == wt::WtWorldPageStatus::Ok,
			"compacted world page validation failed"
		);
	}
	const wt::WtDependencyEntry *previous_audit = nullptr;
	const wt::WtDependencyEntry *journal_audit = nullptr;
	for (const wt::WtDependencyEntry &entry : view.dependencies) {
		if (entry.label == wt::kWtPreviousWorldAuditLabel) {
			previous_audit = &entry;
		}
		if (entry.label == wt::kWtEditJournalAuditLabel) {
			journal_audit = &entry;
		}
	}
	check(
		previous_audit != nullptr && journal_audit != nullptr &&
			previous_audit->content_hash ==
				compacted.audit.previous_world_hash &&
			journal_audit->content_hash ==
				compacted.audit.edit_journal_hash &&
			compacted.audit.previous_source_revision == 100 &&
			compacted.audit.new_source_revision == 101 &&
			compacted.audit.initial_world_revision == 0 &&
			compacted.audit.compacted_world_revision == 2 &&
			compacted.audit.new_world_hash ==
				wt::wt_sha256(
					compacted.world_bytes.data(),
					compacted.world_bytes.size()
				),
		"compaction audit mismatch"
	);
	std::reverse(pages.begin(), pages.end());
	check_sample_equivalence(pages, journal, compacted);

	wt::WtEditJournal next_journal(1, 1, 4096);
	next_journal.reset(view.source_revision, view.world_revision);
	wt::WtEditTransaction next;
	next.source_revision = 101;
	next.transaction_id = id(90);
	next.base_revision = 2;
	next.committed_revision = 3;
	next.commands = { sphere(100, 0, 3, 8, -1.0F) };
	std::vector<std::uint8_t> segment;
	check(
		next_journal.append(next, segment) == wt::WtEditJournalStatus::Ok &&
			next_journal.current_world_revision() == 3,
		"post-compaction journal did not continue persisted revision"
	);
}

void test_failures() {
	const std::vector<wt::WtBakedChunkPage> pages = bake_pages();
	const std::vector<std::uint8_t> world = write_world(pages);
	const wt::WtEditJournal journal = make_journal();
	wt::WtCompactedSnapshot output;
	check(
		wt::wt_compact_snapshot(
			{ world.data(), world.size() },
			pages,
			journal,
			100,
			2,
			output
		) == wt::WtSnapshotCompactionStatus::InvalidInput,
		"non-increasing compaction source revision accepted"
	);
	check(
		wt::wt_compact_snapshot(
			{ world.data(), world.size() },
			pages,
			journal,
			101,
			1,
			output
		) == wt::WtSnapshotCompactionStatus::PageCapacityExceeded,
		"compaction page capacity overflow accepted"
	);
	std::vector<wt::WtBakedChunkPage> missing = pages;
	missing.pop_back();
	check(
		wt::wt_compact_snapshot(
			{ world.data(), world.size() },
			missing,
			journal,
			101,
			2,
			output
		) == wt::WtSnapshotCompactionStatus::PageMismatch,
		"missing compaction page accepted"
	);
	std::vector<wt::WtBakedChunkPage> corrupt = pages;
	corrupt[0].bytes.back() ^= 0x80U;
	check(
		wt::wt_compact_snapshot(
			{ world.data(), world.size() },
			corrupt,
			journal,
			101,
			2,
			output
		) == wt::WtSnapshotCompactionStatus::PageMismatch,
		"corrupt compaction page accepted"
	);
	const wt::WtEditJournal wrong_source = make_journal(99, 0);
	check(
		wt::wt_compact_snapshot(
			{ world.data(), world.size() },
			pages,
			wrong_source,
			101,
			2,
			output
		) == wt::WtSnapshotCompactionStatus::JournalMismatch,
		"wrong-source compaction journal accepted"
	);
	wt::WtEditJournal empty(1, 1, 4096);
	empty.reset(100, 0);
	check(
		wt::wt_compact_snapshot(
			{ world.data(), world.size() },
			pages,
			empty,
			101,
			2,
			output
		) == wt::WtSnapshotCompactionStatus::InvalidInput,
		"empty compaction journal accepted"
	);
}

} // namespace

int main() {
	wt::WtCompactedSnapshot compacted;
	test_compaction(compacted);
	test_failures();
	if (failure_count != 0) {
		std::fprintf(stderr, "M4_COMPACTION_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::vector<std::uint8_t> evidence = compacted.world_bytes;
	for (const wt::WtBakedChunkPage &page : compacted.pages) {
		evidence.insert(evidence.end(), page.bytes.begin(), page.bytes.end());
	}
	std::printf("M4_COMPACTION_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"M4_COMPACTION_PASS pages=2 compacted_revision=2 failure_cases=6\n"
	);
	return 0;
}
