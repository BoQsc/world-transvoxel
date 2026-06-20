#pragma once

#include "core/wt_chunk_key.h"

#include <cstddef>
#include <vector>

namespace world_transvoxel {

enum class WtLodMapStatus : std::uint8_t {
	Ok,
	InvalidKey,
	DuplicateKey,
	CapacityExceeded,
	OverlappingLeaves,
	LodDifferenceExceeded,
};

struct WtLodMapEntry {
	WtChunkKey key;
	std::uint8_t transition_mask = 0;
};

class WtLodMap {
public:
	explicit WtLodMap(std::size_t capacity);

	WtLodMapStatus set_active_chunks(const std::vector<WtChunkKey> &keys);
	const std::vector<WtLodMapEntry> &get_entries() const noexcept;
	const WtLodMapEntry *find(const WtChunkKey &key) const noexcept;
	std::size_t capacity() const noexcept;

private:
	std::size_t capacity_ = 0;
	std::vector<WtLodMapEntry> entries_;
};

} // namespace world_transvoxel
