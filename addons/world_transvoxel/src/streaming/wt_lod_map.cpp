#include "streaming/wt_lod_map.h"

#include <algorithm>
#include <limits>

namespace world_transvoxel {
namespace {

const WtLodMapEntry *find_entry(
	const std::vector<WtLodMapEntry> &entries,
	const WtChunkKey &key
) noexcept {
	const auto iterator = std::lower_bound(
		entries.begin(), entries.end(), key,
		[](const WtLodMapEntry &entry, const WtChunkKey &value) {
			return entry.key < value;
		}
	);
	return iterator != entries.end() && iterator->key == key ?
		&*iterator : nullptr;
}

WtLodMapEntry *find_entry(
	std::vector<WtLodMapEntry> &entries,
	const WtChunkKey &key
) noexcept {
	return const_cast<WtLodMapEntry *>(find_entry(
		static_cast<const std::vector<WtLodMapEntry> &>(entries), key
	));
}

bool adjacent_key(
	const WtChunkKey &key,
	WtChunkFace face,
	WtChunkKey &adjacent
) noexcept {
	adjacent = key;
	std::int32_t *axis = nullptr;
	bool positive = false;
	switch (face) {
		case WtChunkFace::NegativeX: axis = &adjacent.x; break;
		case WtChunkFace::PositiveX: axis = &adjacent.x; positive = true; break;
		case WtChunkFace::NegativeY: axis = &adjacent.y; break;
		case WtChunkFace::PositiveY: axis = &adjacent.y; positive = true; break;
		case WtChunkFace::NegativeZ: axis = &adjacent.z; break;
		case WtChunkFace::PositiveZ: axis = &adjacent.z; positive = true; break;
	}
	if ((positive && *axis == std::numeric_limits<std::int32_t>::max()) ||
		(!positive && *axis == std::numeric_limits<std::int32_t>::min())) {
		return false;
	}
	*axis += positive ? 1 : -1;
	return true;
}

} // namespace

WtLodMap::WtLodMap(std::size_t capacity) : capacity_(capacity) {
	entries_.reserve(capacity);
}

WtLodMapStatus WtLodMap::set_active_chunks(const std::vector<WtChunkKey> &keys) {
	if (keys.size() > capacity_) {
		return WtLodMapStatus::CapacityExceeded;
	}
	std::vector<WtLodMapEntry> candidate;
	candidate.reserve(capacity_);
	for (const WtChunkKey &key : keys) {
		if (!wt_is_valid_chunk_key(key)) {
			return WtLodMapStatus::InvalidKey;
		}
		candidate.push_back({ key, 0 });
	}
	std::sort(candidate.begin(), candidate.end(), [](const auto &a, const auto &b) {
		return a.key < b.key;
	});
	for (std::size_t index = 1; index < candidate.size(); ++index) {
		if (candidate[index - 1].key == candidate[index].key) {
			return WtLodMapStatus::DuplicateKey;
		}
	}

	// Dyadic chunk leaves can overlap only when one key is an ancestor of the
	// other. Checking the ancestor chain is exact and avoids an all-pairs bounds
	// scan for every viewer update.
	for (const WtLodMapEntry &entry : candidate) {
		WtChunkKey ancestor = entry.key;
		while (ancestor.lod < kWtMaximumLod) {
			ancestor = wt_parent_chunk_key(ancestor);
			if (find_entry(candidate, ancestor) != nullptr) {
				return WtLodMapStatus::OverlappingLeaves;
			}
		}
	}

	// Every mixed-LOD face is discovered from its finer leaf. The adjacent
	// same-LOD cell identifies the unique coarser ancestor on the other side.
	// A fine leaf therefore needs at most six short ancestor walks regardless
	// of the total active set size.
	for (const WtLodMapEntry &entry : candidate) {
		for (std::uint8_t face_index = 0; face_index < 6U; ++face_index) {
			const WtChunkFace face = static_cast<WtChunkFace>(face_index);
			WtChunkKey neighbor;
			if (!adjacent_key(entry.key, face, neighbor)) continue;
			for (;;) {
				WtLodMapEntry *neighbor_entry = find_entry(candidate, neighbor);
				if (neighbor_entry != nullptr) {
					const std::uint8_t difference =
						neighbor_entry->key.lod - entry.key.lod;
					if (difference > 1U) {
						return WtLodMapStatus::LodDifferenceExceeded;
					}
					if (difference == 1U) {
						neighbor_entry->transition_mask |=
							wt_face_bit(wt_opposite_face(face));
					}
					break;
				}
				if (neighbor.lod == kWtMaximumLod) break;
				neighbor = wt_parent_chunk_key(neighbor);
			}
		}
	}

	entries_.swap(candidate);
	return WtLodMapStatus::Ok;
}

const std::vector<WtLodMapEntry> &WtLodMap::get_entries() const noexcept {
	return entries_;
}

const WtLodMapEntry *WtLodMap::find(const WtChunkKey &key) const noexcept {
	const auto iterator = std::lower_bound(
		entries_.begin(),
		entries_.end(),
		key,
		[](const WtLodMapEntry &entry, const WtChunkKey &value) {
			return entry.key < value;
		}
	);
	return iterator != entries_.end() && iterator->key == key ? &*iterator : nullptr;
}

std::size_t WtLodMap::capacity() const noexcept {
	return capacity_;
}

} // namespace world_transvoxel
