#include "core/wt_chunk_brick.h"

#include <tuple>

namespace world_transvoxel {

bool WtRegularBrickKey::operator==(
	const WtRegularBrickKey &other
) const noexcept {
	return chunk == other.chunk && index == other.index;
}

bool WtRegularBrickKey::operator!=(
	const WtRegularBrickKey &other
) const noexcept {
	return !(*this == other);
}

bool WtRegularBrickKey::operator<(
	const WtRegularBrickKey &other
) const noexcept {
	return std::tie(chunk, index) < std::tie(other.chunk, other.index);
}

bool WtRegularBrickTransitionFace::operator==(
	const WtRegularBrickTransitionFace &other
) const noexcept {
	return coarse_brick == other.coarse_brick && face == other.face;
}

bool wt_is_valid_regular_brick_key(const WtRegularBrickKey &key) noexcept {
	return wt_is_valid_chunk_key(key.chunk) && key.index < kWtRegularBrickCount;
}

WtGridPoint wt_regular_brick_coordinate(std::uint8_t index) noexcept {
	if (index >= kWtRegularBrickCount) return {};
	return {
		static_cast<std::int64_t>(index & 1U),
		static_cast<std::int64_t>((index >> 1U) & 1U),
		static_cast<std::int64_t>((index >> 2U) & 1U),
	};
}

WtChunkBounds wt_regular_brick_bounds(const WtRegularBrickKey &key) noexcept {
	if (!wt_is_valid_regular_brick_key(key)) return {};
	const WtChunkBounds chunk = wt_chunk_bounds(key.chunk);
	const WtGridPoint coordinate = wt_regular_brick_coordinate(key.index);
	const std::int64_t extent =
		kWtRegularBrickCellsPerAxis * wt_lod_cell_size(key.chunk.lod);
	const WtGridPoint minimum = {
		chunk.minimum.x + coordinate.x * extent,
		chunk.minimum.y + coordinate.y * extent,
		chunk.minimum.z + coordinate.z * extent,
	};
	return {
		minimum,
		{ minimum.x + extent, minimum.y + extent, minimum.z + extent },
	};
}

WtRegularBrickSet wt_regular_bricks_from_mask(
	const WtChunkKey &chunk,
	std::uint8_t mask
) noexcept {
	WtRegularBrickSet result;
	if (!wt_is_valid_chunk_key(chunk)) return result;
	result.mask = mask;
	for (std::uint8_t index = 0; index < kWtRegularBrickCount; ++index) {
		if ((mask & static_cast<std::uint8_t>(1U << index)) == 0) continue;
		result.keys[result.count++] = { chunk, index };
	}
	return result;
}

std::uint8_t wt_regular_brick_mask_for_sample_bounds(
	const WtChunkKey &chunk,
	const WtGridPoint &dirty_minimum,
	const WtGridPoint &dirty_maximum
) noexcept {
	if (!wt_is_valid_chunk_key(chunk) ||
			dirty_minimum.x > dirty_maximum.x ||
			dirty_minimum.y > dirty_maximum.y ||
			dirty_minimum.z > dirty_maximum.z) return 0;
	std::uint8_t mask = 0;
	for (std::uint8_t index = 0; index < kWtRegularBrickCount; ++index) {
		const WtChunkBounds brick = wt_regular_brick_bounds({ chunk, index });
		if (dirty_maximum.x >= brick.minimum.x &&
				dirty_minimum.x <= brick.maximum.x &&
				dirty_maximum.y >= brick.minimum.y &&
				dirty_minimum.y <= brick.maximum.y &&
				dirty_maximum.z >= brick.minimum.z &&
				dirty_minimum.z <= brick.maximum.z) {
			mask |= static_cast<std::uint8_t>(1U << index);
		}
	}
	return mask;
}

bool wt_direct_child_parent_brick(
	const WtChunkKey &child,
	const WtChunkKey &parent,
	WtRegularBrickKey &brick
) noexcept {
	if (!wt_is_valid_chunk_key(child) || !wt_is_valid_chunk_key(parent) ||
			child.lod >= kWtMaximumLod || child.lod + 1U != parent.lod ||
			wt_parent_chunk_key(child) != parent) return false;
	const std::int64_t local_x = static_cast<std::int64_t>(child.x) -
		static_cast<std::int64_t>(parent.x) * 2;
	const std::int64_t local_y = static_cast<std::int64_t>(child.y) -
		static_cast<std::int64_t>(parent.y) * 2;
	const std::int64_t local_z = static_cast<std::int64_t>(child.z) -
		static_cast<std::int64_t>(parent.z) * 2;
	if (local_x < 0 || local_x > 1 || local_y < 0 || local_y > 1 ||
			local_z < 0 || local_z > 1) return false;
	brick = {
		parent,
		static_cast<std::uint8_t>(local_x + local_y * 2 + local_z * 4),
	};
	return true;
}

WtRegularBrickTransitionSet wt_regular_brick_transition_faces(
	const WtChunkKey &chunk,
	std::uint8_t visible_mask
) noexcept {
	WtRegularBrickTransitionSet result;
	if (!wt_is_valid_chunk_key(chunk) || visible_mask == 0 ||
			visible_mask == kWtAllRegularBricksMask) return result;
	for (std::uint8_t axis = 0; axis < 3; ++axis) {
		const std::uint8_t step = static_cast<std::uint8_t>(1U << axis);
		for (std::uint8_t lower = 0; lower < kWtRegularBrickCount; ++lower) {
			if ((lower & step) != 0) continue;
			const std::uint8_t upper = static_cast<std::uint8_t>(lower | step);
			const bool lower_visible =
				(visible_mask & static_cast<std::uint8_t>(1U << lower)) != 0;
			const bool upper_visible =
				(visible_mask & static_cast<std::uint8_t>(1U << upper)) != 0;
			if (lower_visible == upper_visible) continue;
			result.faces[result.count++] = {
				{ chunk, lower_visible ? lower : upper },
				static_cast<WtChunkFace>(axis * 2U + (lower_visible ? 1U : 0U)),
			};
		}
	}
	return result;
}

} // namespace world_transvoxel
