#pragma once

#include "core/wt_chunk_key.h"
#include "storage/wt_binary_io.h"
#include "storage/wt_procedural_world_descriptor.h"

#include <cstdint>
#include <vector>

namespace world_transvoxel {

// Views refer to the caller-owned input. Keep the atlas bytes alive until GPU
// upload is complete. An atlas is only a pristine-source base; journal edits
// must be incorporated before an edited world can expose this coverage.
struct WtBaseCoverageRootView {
	WtChunkKey key;
	WtByteView vertices;
	WtByteView indices;
	std::uint32_t vertex_count = 0;
	std::uint32_t index_count = 0;
	bool proven_empty = false;
};

struct WtBaseCoverageAtlasView {
	std::vector<WtBaseCoverageRootView> roots;
	std::uint64_t source_revision = 0;
	std::uint8_t lod = 0;
};

enum class WtBaseCoverageAtlasStatus : std::uint8_t {
	Ok,
	InvalidInput,
	CapacityExceeded,
	Truncated,
	UnsupportedVersion,
	SourceMismatch,
	IncompleteInventory,
	InvalidRecord,
	HashMismatch,
};

// Fail closed unless *every* expected root has a valid mesh or an explicit
// empty certificate. On failure output is cleared and cannot imply coverage.
WtBaseCoverageAtlasStatus wt_open_base_coverage_atlas(
	WtByteView bytes,
	const WtProceduralWorldDescriptor &expected_source,
	std::uint8_t expected_lod,
	const std::vector<WtChunkKey> &expected_roots,
	WtBaseCoverageAtlasView &output
) noexcept;

} // namespace world_transvoxel
