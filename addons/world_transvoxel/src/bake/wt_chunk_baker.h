#pragma once

#include "meshing/wt_chunk_mesher.h"
#include "storage/wt_chunk_page.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

struct WtBakedChunkPage {
	WtChunkKey key;
	WtHash256 content_hash{};
	std::vector<std::uint8_t> bytes;
};

enum class WtChunkBakeStatus : std::uint8_t {
	Ok,
	InvalidInput,
	DuplicateKey,
	PageCapacityExceeded,
	SampleSourceFailure,
	PageWriteFailure,
};

class WtChunkBaker {
public:
	explicit WtChunkBaker(std::size_t page_capacity) noexcept;

	WtChunkBakeStatus bake(
		const std::vector<WtChunkKey> &keys,
		std::uint64_t source_revision,
		const WtChunkSampleSource &source,
		std::vector<WtBakedChunkPage> &output
	) const;

private:
	std::size_t page_capacity_ = 0;
};

} // namespace world_transvoxel
