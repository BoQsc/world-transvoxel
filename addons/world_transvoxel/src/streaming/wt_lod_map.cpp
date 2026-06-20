#include "streaming/wt_lod_map.h"

#include <algorithm>

namespace world_transvoxel {
namespace {

bool intervals_overlap(
	std::int64_t a_minimum,
	std::int64_t a_maximum,
	std::int64_t b_minimum,
	std::int64_t b_maximum
) noexcept {
	return a_minimum < b_maximum && b_minimum < a_maximum;
}

bool volumes_overlap(const WtChunkBounds &a, const WtChunkBounds &b) noexcept {
	return intervals_overlap(a.minimum.x, a.maximum.x, b.minimum.x, b.maximum.x) &&
		intervals_overlap(a.minimum.y, a.maximum.y, b.minimum.y, b.maximum.y) &&
		intervals_overlap(a.minimum.z, a.maximum.z, b.minimum.z, b.maximum.z);
}

bool face_neighbor(
	const WtChunkBounds &a,
	const WtChunkBounds &b,
	WtChunkFace &a_face
) noexcept {
	if (a.maximum.x == b.minimum.x &&
		intervals_overlap(a.minimum.y, a.maximum.y, b.minimum.y, b.maximum.y) &&
		intervals_overlap(a.minimum.z, a.maximum.z, b.minimum.z, b.maximum.z)) {
		a_face = WtChunkFace::PositiveX;
		return true;
	}
	if (a.minimum.x == b.maximum.x &&
		intervals_overlap(a.minimum.y, a.maximum.y, b.minimum.y, b.maximum.y) &&
		intervals_overlap(a.minimum.z, a.maximum.z, b.minimum.z, b.maximum.z)) {
		a_face = WtChunkFace::NegativeX;
		return true;
	}
	if (a.maximum.y == b.minimum.y &&
		intervals_overlap(a.minimum.x, a.maximum.x, b.minimum.x, b.maximum.x) &&
		intervals_overlap(a.minimum.z, a.maximum.z, b.minimum.z, b.maximum.z)) {
		a_face = WtChunkFace::PositiveY;
		return true;
	}
	if (a.minimum.y == b.maximum.y &&
		intervals_overlap(a.minimum.x, a.maximum.x, b.minimum.x, b.maximum.x) &&
		intervals_overlap(a.minimum.z, a.maximum.z, b.minimum.z, b.maximum.z)) {
		a_face = WtChunkFace::NegativeY;
		return true;
	}
	if (a.maximum.z == b.minimum.z &&
		intervals_overlap(a.minimum.x, a.maximum.x, b.minimum.x, b.maximum.x) &&
		intervals_overlap(a.minimum.y, a.maximum.y, b.minimum.y, b.maximum.y)) {
		a_face = WtChunkFace::PositiveZ;
		return true;
	}
	if (a.minimum.z == b.maximum.z &&
		intervals_overlap(a.minimum.x, a.maximum.x, b.minimum.x, b.maximum.x) &&
		intervals_overlap(a.minimum.y, a.maximum.y, b.minimum.y, b.maximum.y)) {
		a_face = WtChunkFace::NegativeZ;
		return true;
	}
	return false;
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

	for (std::size_t a_index = 0; a_index < candidate.size(); ++a_index) {
		const WtChunkBounds a_bounds = wt_chunk_bounds(candidate[a_index].key);
		for (std::size_t b_index = a_index + 1; b_index < candidate.size(); ++b_index) {
			const WtChunkBounds b_bounds = wt_chunk_bounds(candidate[b_index].key);
			if (volumes_overlap(a_bounds, b_bounds)) {
				return WtLodMapStatus::OverlappingLeaves;
			}
			WtChunkFace a_face = WtChunkFace::NegativeX;
			if (!face_neighbor(a_bounds, b_bounds, a_face)) {
				continue;
			}
			const int lod_difference =
				static_cast<int>(candidate[a_index].key.lod) -
				static_cast<int>(candidate[b_index].key.lod);
			if (lod_difference < -1 || lod_difference > 1) {
				return WtLodMapStatus::LodDifferenceExceeded;
			}
			if (lod_difference > 0) {
				candidate[a_index].transition_mask |= wt_face_bit(a_face);
			} else if (lod_difference < 0) {
				candidate[b_index].transition_mask |= wt_face_bit(wt_opposite_face(a_face));
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
