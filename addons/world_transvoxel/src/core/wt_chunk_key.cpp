#include "core/wt_chunk_key.h"

#include <tuple>

namespace world_transvoxel {
namespace {

std::int32_t floor_divide_by_two(std::int32_t value) noexcept {
	if (value >= 0) {
		return value / 2;
	}
	return -static_cast<std::int32_t>(
		(-static_cast<std::int64_t>(value) + 1) / 2
	);
}

} // namespace

bool WtGridPoint::operator==(const WtGridPoint &other) const noexcept {
	return x == other.x && y == other.y && z == other.z;
}

bool WtGridPoint::operator!=(const WtGridPoint &other) const noexcept {
	return !(*this == other);
}

bool WtChunkKey::operator==(const WtChunkKey &other) const noexcept {
	return x == other.x && y == other.y && z == other.z && lod == other.lod;
}

bool WtChunkKey::operator!=(const WtChunkKey &other) const noexcept {
	return !(*this == other);
}

bool WtChunkKey::operator<(const WtChunkKey &other) const noexcept {
	return std::tie(lod, z, y, x) < std::tie(other.lod, other.z, other.y, other.x);
}

bool wt_is_valid_chunk_key(const WtChunkKey &key) noexcept {
	return key.lod <= kWtMaximumLod;
}

std::int64_t wt_lod_cell_size(std::uint8_t lod) noexcept {
	return lod <= kWtMaximumLod ? (std::int64_t{ 1 } << lod) : 0;
}

std::int64_t wt_chunk_extent(std::uint8_t lod) noexcept {
	return kWtChunkCellsPerAxis * wt_lod_cell_size(lod);
}

WtChunkBounds wt_chunk_bounds(const WtChunkKey &key) noexcept {
	const std::int64_t extent = wt_chunk_extent(key.lod);
	const WtGridPoint minimum = {
		static_cast<std::int64_t>(key.x) * extent,
		static_cast<std::int64_t>(key.y) * extent,
		static_cast<std::int64_t>(key.z) * extent,
	};
	return {
		minimum,
		{ minimum.x + extent, minimum.y + extent, minimum.z + extent },
	};
}

WtChunkKey wt_parent_chunk_key(const WtChunkKey &key) noexcept {
	if (!wt_is_valid_chunk_key(key) || key.lod == kWtMaximumLod) {
		return key;
	}
	return {
		floor_divide_by_two(key.x),
		floor_divide_by_two(key.y),
		floor_divide_by_two(key.z),
		static_cast<std::uint8_t>(key.lod + 1),
	};
}

WtChunkFace wt_opposite_face(WtChunkFace face) noexcept {
	return static_cast<WtChunkFace>(static_cast<std::uint8_t>(face) ^ 1U);
}

std::uint8_t wt_face_bit(WtChunkFace face) noexcept {
	const std::uint8_t index = static_cast<std::uint8_t>(face);
	return index < 6 ? static_cast<std::uint8_t>(1U << index) : 0;
}

} // namespace world_transvoxel
