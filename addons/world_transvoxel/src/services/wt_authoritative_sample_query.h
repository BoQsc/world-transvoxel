#pragma once

#include "core/wt_chunk_key.h"
#include "meshing/wt_chunk_mesher.h"

#include <cstdint>

namespace world_transvoxel {

class WtAsyncStorageService;
class WtEditJournal;

enum class WtAuthoritativeSampleQueryStatus : std::uint8_t {
	Ok,
	InvalidPoint,
	NotFound,
	StorageFailure,
	PageFailure,
	EditReplayFailure,
	ConflictingSamples,
};

struct WtAuthoritativeSample {
	WtGridPoint point;
	std::uint8_t lod = 0;
	WtScalarSample sample;
	std::uint64_t source_revision = 0;
	std::uint64_t world_revision = 0;
	std::size_t agreeing_page_count = 0;
};

WtAuthoritativeSampleQueryStatus wt_query_authoritative_sample(
	const WtGridPoint &point,
	std::uint8_t lod,
	WtAsyncStorageService &storage,
	const WtEditJournal &journal,
	std::uint64_t initial_world_revision,
	WtAuthoritativeSample &output
);

const char *wt_authoritative_sample_query_status_message(
	WtAuthoritativeSampleQueryStatus status
) noexcept;

} // namespace world_transvoxel
