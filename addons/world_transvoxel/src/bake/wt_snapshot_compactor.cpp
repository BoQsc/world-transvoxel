#include "bake/wt_snapshot_compactor.h"

#include "editing/wt_chunk_edit_state.h"

#include <algorithm>
#include <utility>

namespace world_transvoxel {
namespace {

bool is_compaction_audit(const WtDependencyEntry &entry) {
	return entry.kind == WtDependencyKind::SourceAsset &&
		(entry.label == kWtPreviousWorldAuditLabel ||
			entry.label == kWtEditJournalAuditLabel);
}

} // namespace

WtSnapshotCompactionStatus wt_compact_snapshot(
	WtByteView previous_world_bytes,
	const std::vector<WtBakedChunkPage> &source_pages,
	const WtEditJournal &journal,
	std::uint64_t new_source_revision,
	std::size_t page_capacity,
	WtCompactedSnapshot &output
) {
	output = {};
	if (!journal.initialized() ||
		journal.transaction_count() == 0 ||
		source_pages.empty() ||
		source_pages.size() > page_capacity) {
		return source_pages.size() > page_capacity ?
			WtSnapshotCompactionStatus::PageCapacityExceeded :
			WtSnapshotCompactionStatus::InvalidInput;
	}
	WtWorldManifestView previous_world;
	if (wt_open_world_manifest(previous_world_bytes, previous_world) !=
		WtWorldManifestStatus::Ok) {
		return WtSnapshotCompactionStatus::WorldFailure;
	}
	if (new_source_revision <= previous_world.source_revision) {
		return WtSnapshotCompactionStatus::InvalidInput;
	}
	if (journal.source_revision() != previous_world.source_revision ||
		journal.initial_world_revision() != previous_world.world_revision) {
		return WtSnapshotCompactionStatus::JournalMismatch;
	}
	if (source_pages.size() != previous_world.pages.size()) {
		return WtSnapshotCompactionStatus::PageMismatch;
	}
	std::vector<const WtBakedChunkPage *> ordered_pages;
	ordered_pages.reserve(source_pages.size());
	for (const WtBakedChunkPage &page : source_pages) {
		ordered_pages.push_back(&page);
	}
	std::sort(
		ordered_pages.begin(),
		ordered_pages.end(),
		[](const WtBakedChunkPage *left, const WtBakedChunkPage *right) {
			return left->key < right->key;
		}
	);

	WtCompactedSnapshot compacted;
	compacted.pages.reserve(source_pages.size());
	for (std::size_t index = 0; index < ordered_pages.size(); ++index) {
		const WtBakedChunkPage &source = *ordered_pages[index];
		if (source.key != previous_world.pages[index].key ||
			source.content_hash !=
				wt_sha256(source.bytes.data(), source.bytes.size()) ||
			wt_validate_world_page(
				previous_world,
				source.key,
				{ source.bytes.data(), source.bytes.size() }
			) != WtWorldPageStatus::Ok) {
			return WtSnapshotCompactionStatus::PageMismatch;
		}
		WtChunkPageView page_view;
		WtChunkPage page;
		if (wt_open_chunk_page(
				{ source.bytes.data(), source.bytes.size() },
				page_view
			) != WtChunkPageStatus::Ok ||
			wt_decode_chunk_page(page_view, page) != WtChunkPageStatus::Ok) {
			return WtSnapshotCompactionStatus::PageMismatch;
		}
		WtChunkEditState edit_state;
		if (edit_state.initialize(
				std::move(page),
				previous_world.source_revision,
				previous_world.world_revision
			) != WtChunkEditStatus::Ok ||
			journal.replay(edit_state) != WtEditJournalStatus::Ok ||
			edit_state.current_world_revision() !=
				journal.current_world_revision()) {
			return WtSnapshotCompactionStatus::EditReplayFailure;
		}
		WtChunkPage compacted_page = edit_state.page();
		compacted_page.metadata.source_revision = new_source_revision;
		WtBakedChunkPage baked;
		baked.key = compacted_page.metadata.key;
		if (wt_write_chunk_page(compacted_page, baked.bytes) !=
			WtChunkPageStatus::Ok) {
			return WtSnapshotCompactionStatus::PageWriteFailure;
		}
		baked.content_hash = wt_sha256(
			baked.bytes.data(),
			baked.bytes.size()
		);
		compacted.pages.push_back(std::move(baked));
	}

	std::vector<std::uint8_t> journal_bytes;
	if (journal.save(journal_bytes) != WtEditJournalStatus::Ok) {
		return WtSnapshotCompactionStatus::JournalMismatch;
	}
	const WtHash256 previous_world_hash = wt_sha256(
		previous_world_bytes.data,
		previous_world_bytes.size
	);
	const WtHash256 journal_hash = wt_sha256(
		journal_bytes.data(),
		journal_bytes.size()
	);
	std::vector<WtDependencyEntry> dependencies =
		previous_world.dependencies;
	dependencies.erase(
		std::remove_if(
			dependencies.begin(),
			dependencies.end(),
			is_compaction_audit
		),
		dependencies.end()
	);
	dependencies.push_back({
		WtDependencyKind::SourceAsset,
		kWtPreviousWorldAuditLabel,
		"",
		previous_world_hash,
	});
	dependencies.push_back({
		WtDependencyKind::SourceAsset,
		kWtEditJournalAuditLabel,
		"",
		journal_hash,
	});

	WtWorldManifest manifest;
	manifest.source_revision = new_source_revision;
	manifest.world_revision = journal.current_world_revision();
	manifest.configuration_hash = previous_world.configuration_hash;
	manifest.dependencies = std::move(dependencies);
	for (const WtBakedChunkPage &page : compacted.pages) {
		manifest.pages.push_back({
			page.key,
			page.bytes.size(),
			page.content_hash,
		});
	}
	if (wt_write_world_manifest(manifest, compacted.world_bytes) !=
		WtWorldManifestStatus::Ok) {
		return WtSnapshotCompactionStatus::ManifestWriteFailure;
	}
	compacted.audit = {
		previous_world.source_revision,
		new_source_revision,
		previous_world.world_revision,
		journal.current_world_revision(),
		previous_world_hash,
		journal_hash,
		wt_sha256(compacted.world_bytes.data(), compacted.world_bytes.size()),
	};
	output = std::move(compacted);
	return WtSnapshotCompactionStatus::Ok;
}

} // namespace world_transvoxel
