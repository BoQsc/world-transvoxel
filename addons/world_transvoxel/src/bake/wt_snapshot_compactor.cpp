#include "bake/wt_snapshot_compactor.h"

#include "editing/wt_chunk_edit_state.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace world_transvoxel {
namespace {

bool is_compaction_audit(const WtDependencyEntry &entry) {
	return entry.kind == WtDependencyKind::SourceAsset &&
		(entry.label == kWtPreviousWorldAuditLabel ||
			entry.label == kWtEditJournalAuditLabel);
}

std::int64_t floor_divide(
	std::int64_t numerator,
	std::int64_t denominator
) noexcept {
	std::int64_t quotient = numerator / denominator;
	if (numerator % denominator < 0) {
		--quotient;
	}
	return quotient;
}

class CompactionLod0SampleSource final : public WtChunkSampleSource {
public:
	CompactionLod0SampleSource(
		const std::vector<const WtBakedChunkPage *> &pages,
		const WtWorldManifestView &world,
		const WtEditJournal &journal
	) noexcept :
			pages_(pages),
			world_(world),
			journal_(journal) {
		cache_.reserve(kCacheCapacity);
	}

	bool sample(
		const WtGridPoint &point,
		WtScalarSample &output
	) const noexcept override {
		constexpr std::array<int, 3> offsets = { 0, -1, 1 };
		const std::int64_t base[3] = {
			floor_divide(point.x, kWtChunkCellsPerAxis),
			floor_divide(point.y, kWtChunkCellsPerAxis),
			floor_divide(point.z, kWtChunkCellsPerAxis),
		};
		for (int z_offset : offsets) {
			for (int y_offset : offsets) {
				for (int x_offset : offsets) {
					const std::int64_t x = base[0] + x_offset;
					const std::int64_t y = base[1] + y_offset;
					const std::int64_t z = base[2] + z_offset;
					if (x < std::numeric_limits<std::int32_t>::min() ||
						x > std::numeric_limits<std::int32_t>::max() ||
						y < std::numeric_limits<std::int32_t>::min() ||
						y > std::numeric_limits<std::int32_t>::max() ||
						z < std::numeric_limits<std::int32_t>::min() ||
						z > std::numeric_limits<std::int32_t>::max()) {
						continue;
					}
					const WtChunkKey key = {
						static_cast<std::int32_t>(x),
						static_cast<std::int32_t>(y),
						static_cast<std::int32_t>(z),
						0,
					};
					const WtChunkPage *page = find_or_load(key);
					if (page != nullptr &&
						wt_sample_chunk_page(*page, point, output)) {
						return true;
					}
				}
			}
		}
		return false;
	}

private:
	struct CacheEntry {
		WtChunkKey key;
		WtChunkPage page;
		std::uint64_t access = 0;
	};

	const WtBakedChunkPage *find_baked(
		const WtChunkKey &key
	) const noexcept {
		const auto found = std::lower_bound(
			pages_.begin(),
			pages_.end(),
			key,
			[](const WtBakedChunkPage *left, const WtChunkKey &right) {
				return left->key < right;
			}
		);
		return found != pages_.end() && (*found)->key == key ?
			*found : nullptr;
	}

	const WtChunkPage *find_or_load(
		const WtChunkKey &key
	) const {
		for (CacheEntry &entry : cache_) {
			if (entry.key == key) {
				entry.access = ++next_access_;
				return &entry.page;
			}
		}
		const WtBakedChunkPage *baked = find_baked(key);
		if (baked == nullptr ||
			baked->content_hash != wt_sha256(
				baked->bytes.data(), baked->bytes.size()
			) ||
			wt_validate_world_page(
				world_,
				key,
				{ baked->bytes.data(), baked->bytes.size() }
			) != WtWorldPageStatus::Ok) {
			return nullptr;
		}
		WtChunkPageView view;
		WtChunkPage page;
		if (wt_open_chunk_page(
				{ baked->bytes.data(), baked->bytes.size() }, view
			) != WtChunkPageStatus::Ok ||
			wt_decode_chunk_page(view, page) != WtChunkPageStatus::Ok) {
			return nullptr;
		}
		WtChunkEditState edit_state;
		if (edit_state.initialize(
				std::move(page),
				world_.source_revision,
				world_.world_revision
			) != WtChunkEditStatus::Ok ||
			journal_.replay(edit_state) != WtEditJournalStatus::Ok ||
			edit_state.current_world_revision() !=
				journal_.current_world_revision()) {
			return nullptr;
		}
		if (cache_.size() >= kCacheCapacity) {
			const auto oldest = std::min_element(
				cache_.begin(),
				cache_.end(),
				[](const CacheEntry &left, const CacheEntry &right) {
					return left.access < right.access;
				}
			);
			cache_.erase(oldest);
		}
		cache_.push_back({
			key,
			edit_state.page(),
			++next_access_,
		});
		return &cache_.back().page;
	}

	static constexpr std::size_t kCacheCapacity = 32;
	const std::vector<const WtBakedChunkPage *> &pages_;
	const WtWorldManifestView &world_;
	const WtEditJournal &journal_;
	mutable std::vector<CacheEntry> cache_;
	mutable std::uint64_t next_access_ = 0;
};

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
	CompactionLod0SampleSource lod0_source(
		ordered_pages, previous_world, journal
	);
	WtMultiresolutionVertexScratch surface_shift_scratch;

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
		if (!compacted_page.surface_shift_valid &&
			wt_build_surface_shift_records(
				compacted_page,
				lod0_source,
				surface_shift_scratch
			) != WtSurfaceShiftBuildStatus::Ok) {
			return WtSnapshotCompactionStatus::SurfaceShiftFailure;
		}
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
