#pragma once

#include <cstdint>

namespace world_transvoxel {

constexpr std::uint8_t kWtMaximumLod = 20;
constexpr std::int64_t kWtChunkCellsPerAxis = 16;

enum class WtChunkFace : std::uint8_t {
	NegativeX = 0,
	PositiveX = 1,
	NegativeY = 2,
	PositiveY = 3,
	NegativeZ = 4,
	PositiveZ = 5,
};

struct WtGridPoint {
	std::int64_t x = 0;
	std::int64_t y = 0;
	std::int64_t z = 0;

	bool operator==(const WtGridPoint &other) const noexcept;
	bool operator!=(const WtGridPoint &other) const noexcept;
};

struct WtChunkBounds {
	WtGridPoint minimum;
	WtGridPoint maximum;
};

struct WtChunkKey {
	std::int32_t x = 0;
	std::int32_t y = 0;
	std::int32_t z = 0;
	std::uint8_t lod = 0;

	bool operator==(const WtChunkKey &other) const noexcept;
	bool operator!=(const WtChunkKey &other) const noexcept;
	bool operator<(const WtChunkKey &other) const noexcept;
};

bool wt_is_valid_chunk_key(const WtChunkKey &key) noexcept;
std::int64_t wt_lod_cell_size(std::uint8_t lod) noexcept;
std::int64_t wt_chunk_extent(std::uint8_t lod) noexcept;
WtChunkBounds wt_chunk_bounds(const WtChunkKey &key) noexcept;
WtChunkKey wt_parent_chunk_key(const WtChunkKey &key) noexcept;
WtChunkFace wt_opposite_face(WtChunkFace face) noexcept;
std::uint8_t wt_face_bit(WtChunkFace face) noexcept;

} // namespace world_transvoxel
