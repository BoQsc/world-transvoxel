#pragma once

#include "bake/wt_chunk_baker.h"
#include "editing/wt_edit_journal.h"
#include "storage/wt_world_manifest.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

inline constexpr const char *kWtPreviousWorldAuditLabel =
	"compaction/previous-world";
inline constexpr const char *kWtEditJournalAuditLabel =
	"compaction/edit-journal";

struct WtCompactionAudit {
	std::uint64_t previous_source_revision = 0;
	std::uint64_t new_source_revision = 0;
	std::uint64_t initial_world_revision = 0;
	std::uint64_t compacted_world_revision = 0;
	WtHash256 previous_world_hash{};
	WtHash256 edit_journal_hash{};
	WtHash256 new_world_hash{};
};

struct WtCompactedSnapshot {
	std::vector<std::uint8_t> world_bytes;
	std::vector<WtBakedChunkPage> pages;
	WtCompactionAudit audit;
};

enum class WtSnapshotCompactionStatus : std::uint8_t {
	Ok,
	InvalidInput,
	PageCapacityExceeded,
	WorldFailure,
	JournalMismatch,
	PageMismatch,
	EditReplayFailure,
	SurfaceShiftFailure,
	PageWriteFailure,
	ManifestWriteFailure,
};

WtSnapshotCompactionStatus wt_compact_snapshot(
	WtByteView previous_world_bytes,
	const std::vector<WtBakedChunkPage> &source_pages,
	const WtEditJournal &journal,
	std::uint64_t new_source_revision,
	std::size_t page_capacity,
	WtCompactedSnapshot &output
);

} // namespace world_transvoxel
