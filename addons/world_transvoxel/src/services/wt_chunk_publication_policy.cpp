#include "services/wt_chunk_publication_policy.h"

#include <algorithm>
#include <limits>

namespace world_transvoxel {
namespace {

bool bounds_overlap(
	const WtChunkKey &left,
	const WtChunkKey &right
) noexcept {
	if (!wt_is_valid_chunk_key(left) || !wt_is_valid_chunk_key(right)) {
		return false;
	}
	const WtChunkBounds left_bounds = wt_chunk_bounds(left);
	const WtChunkBounds right_bounds = wt_chunk_bounds(right);
	return left_bounds.minimum.x < right_bounds.maximum.x &&
		right_bounds.minimum.x < left_bounds.maximum.x &&
		left_bounds.minimum.y < right_bounds.maximum.y &&
		right_bounds.minimum.y < left_bounds.maximum.y &&
		left_bounds.minimum.z < right_bounds.maximum.z &&
		right_bounds.minimum.z < left_bounds.maximum.z;
}

bool bounds_share_face(
	const WtChunkKey &left,
	const WtChunkKey &right
) noexcept {
	if (!wt_is_valid_chunk_key(left) || !wt_is_valid_chunk_key(right) ||
			bounds_overlap(left, right)) {
		return false;
	}
	const WtChunkBounds left_bounds = wt_chunk_bounds(left);
	const WtChunkBounds right_bounds = wt_chunk_bounds(right);
	const auto positive_overlap = [](double left_minimum, double left_maximum,
			double right_minimum, double right_maximum) noexcept {
		return left_minimum < right_maximum &&
			right_minimum < left_maximum;
	};
	const bool x_face =
		(left_bounds.maximum.x == right_bounds.minimum.x ||
			right_bounds.maximum.x == left_bounds.minimum.x) &&
		positive_overlap(
			left_bounds.minimum.y,
			left_bounds.maximum.y,
			right_bounds.minimum.y,
			right_bounds.maximum.y
		) && positive_overlap(
			left_bounds.minimum.z,
			left_bounds.maximum.z,
			right_bounds.minimum.z,
			right_bounds.maximum.z
		);
	const bool y_face =
		(left_bounds.maximum.y == right_bounds.minimum.y ||
			right_bounds.maximum.y == left_bounds.minimum.y) &&
		positive_overlap(
			left_bounds.minimum.x,
			left_bounds.maximum.x,
			right_bounds.minimum.x,
			right_bounds.maximum.x
		) && positive_overlap(
			left_bounds.minimum.z,
			left_bounds.maximum.z,
			right_bounds.minimum.z,
			right_bounds.maximum.z
		);
	const bool z_face =
		(left_bounds.maximum.z == right_bounds.minimum.z ||
			right_bounds.maximum.z == left_bounds.minimum.z) &&
		positive_overlap(
			left_bounds.minimum.x,
			left_bounds.maximum.x,
			right_bounds.minimum.x,
			right_bounds.maximum.x
		) && positive_overlap(
			left_bounds.minimum.y,
			left_bounds.maximum.y,
			right_bounds.minimum.y,
			right_bounds.maximum.y
		);
	return x_face || y_face || z_face;
}

bool unsafe_lod_boundary(
	const WtChunkKey &replacement,
	const WtChunkKey &retirement
) noexcept {
	const std::uint8_t lod_gap = replacement.lod > retirement.lod ?
		replacement.lod - retirement.lod : retirement.lod - replacement.lod;
	return lod_gap > 1U && bounds_share_face(replacement, retirement);
}

bool insert_key(std::vector<WtChunkKey> &keys, const WtChunkKey &key) {
	const auto position = std::lower_bound(keys.begin(), keys.end(), key);
	if (position != keys.end() && *position == key) return false;
	keys.insert(position, key);
	return true;
}

bool key_child(
	const WtChunkKey &parent,
	std::int32_t child_x,
	std::int32_t child_y,
	std::int32_t child_z,
	WtChunkKey &child
) noexcept;

class ChunkHierarchyIndex {
public:
	ChunkHierarchyIndex(
		const std::vector<WtChunkKey> &keys,
		std::uint8_t maximum_lod
	) : keys_(keys), maximum_lod_(maximum_lod) {
		closure_.reserve(keys.size() * (maximum_lod_ + 1U));
		for (const WtChunkKey &key : keys) {
			if (!wt_is_valid_chunk_key(key)) continue;
			WtChunkKey ancestor = key;
			closure_.push_back(ancestor);
			while (ancestor.lod < maximum_lod_) {
				ancestor = wt_parent_chunk_key(ancestor);
				closure_.push_back(ancestor);
			}
		}
		std::sort(closure_.begin(), closure_.end());
		closure_.erase(
			std::unique(closure_.begin(), closure_.end()),
			closure_.end()
		);
	}

	void append_overlapping(
		const WtChunkKey &key,
		std::vector<WtChunkKey> &output
	) const {
		if (!wt_is_valid_chunk_key(key)) return;
		WtChunkKey ancestor = key;
		while (true) {
			if (contains(keys_, ancestor)) insert_key(output, ancestor);
			if (ancestor.lod == maximum_lod_) break;
			ancestor = wt_parent_chunk_key(ancestor);
		}
		append_descendants(key, output);
	}

private:
	static bool contains(
		const std::vector<WtChunkKey> &keys,
		const WtChunkKey &key
	) noexcept {
		return std::binary_search(keys.begin(), keys.end(), key);
	}

	void append_descendants(
		const WtChunkKey &parent,
		std::vector<WtChunkKey> &output
	) const {
		if (parent.lod == 0) return;
		for (std::int32_t z = 0; z < 2; ++z) {
			for (std::int32_t y = 0; y < 2; ++y) {
				for (std::int32_t x = 0; x < 2; ++x) {
					WtChunkKey child;
					if (!key_child(parent, x, y, z, child) ||
						!contains(closure_, child)) {
						continue;
					}
					if (contains(keys_, child)) insert_key(output, child);
					append_descendants(child, output);
				}
			}
		}
	}

	const std::vector<WtChunkKey> &keys_;
	std::vector<WtChunkKey> closure_;
	std::uint8_t maximum_lod_ = 0;
};

bool overlaps_any(
	const WtChunkKey &key,
	const std::vector<WtChunkKey> &keys
) noexcept {
	for (const WtChunkKey &candidate : keys) {
		if (bounds_overlap(key, candidate)) return true;
	}
	return false;
}

bool bounds_contains(
	const WtChunkKey &outer,
	const WtChunkKey &inner
) noexcept {
	if (!wt_is_valid_chunk_key(outer) || !wt_is_valid_chunk_key(inner)) {
		return false;
	}
	const WtChunkBounds outer_bounds = wt_chunk_bounds(outer);
	const WtChunkBounds inner_bounds = wt_chunk_bounds(inner);
	return outer_bounds.minimum.x <= inner_bounds.minimum.x &&
		outer_bounds.minimum.y <= inner_bounds.minimum.y &&
		outer_bounds.minimum.z <= inner_bounds.minimum.z &&
		inner_bounds.maximum.x <= outer_bounds.maximum.x &&
		inner_bounds.maximum.y <= outer_bounds.maximum.y &&
		inner_bounds.maximum.z <= outer_bounds.maximum.z;
}

bool key_child(
	const WtChunkKey &parent,
	std::int32_t child_x,
	std::int32_t child_y,
	std::int32_t child_z,
	WtChunkKey &child
) noexcept {
	if (parent.lod == 0 || !wt_is_valid_chunk_key(parent)) return false;
	const std::int64_t x = static_cast<std::int64_t>(parent.x) * 2 + child_x;
	const std::int64_t y = static_cast<std::int64_t>(parent.y) * 2 + child_y;
	const std::int64_t z = static_cast<std::int64_t>(parent.z) * 2 + child_z;
	if (x < std::numeric_limits<std::int32_t>::min() ||
		x > std::numeric_limits<std::int32_t>::max() ||
		y < std::numeric_limits<std::int32_t>::min() ||
		y > std::numeric_limits<std::int32_t>::max() ||
		z < std::numeric_limits<std::int32_t>::min() ||
		z > std::numeric_limits<std::int32_t>::max()) {
		return false;
	}
	child = {
		static_cast<std::int32_t>(x),
		static_cast<std::int32_t>(y),
		static_cast<std::int32_t>(z),
		static_cast<std::uint8_t>(parent.lod - 1),
	};
	return true;
}

bool replacement_set_covers(
	const WtChunkKey &target,
	const std::vector<WtChunkKey> &replacements
) noexcept {
	for (const WtChunkKey &replacement : replacements) {
		if (bounds_contains(replacement, target)) return true;
	}
	if (target.lod == 0) return false;
	for (std::int32_t z = 0; z < 2; ++z) {
		for (std::int32_t y = 0; y < 2; ++y) {
			for (std::int32_t x = 0; x < 2; ++x) {
				WtChunkKey child;
				if (!key_child(target, x, y, z, child) ||
						!overlaps_any(child, replacements) ||
						!replacement_set_covers(child, replacements)) {
					return false;
				}
			}
		}
	}
	return true;
}

} // namespace

bool wt_chunk_replacement_requires_regional_publication(
	const WtChunkKey &replacement,
	const std::vector<WtChunkKey> &pending_retirements
) noexcept {
	for (const WtChunkKey &retirement : pending_retirements) {
		if (bounds_overlap(replacement, retirement)) return true;
	}
	return false;
}

bool wt_build_chunk_publication_region(
	const WtChunkKey &seed_replacement,
	const std::vector<WtChunkKey> &pending_replacements,
	const std::vector<WtChunkKey> &pending_retirements,
	WtChunkPublicationRegion &output
) {
	output = {};
	if (!wt_is_valid_chunk_key(seed_replacement) ||
			!std::binary_search(
				pending_replacements.begin(),
				pending_replacements.end(),
				seed_replacement
			)) {
		return false;
	}
	std::uint8_t maximum_lod = seed_replacement.lod;
	for (const WtChunkKey &key : pending_replacements) {
		maximum_lod = std::max(maximum_lod, key.lod);
	}
	for (const WtChunkKey &key : pending_retirements) {
		maximum_lod = std::max(maximum_lod, key.lod);
	}
	const ChunkHierarchyIndex replacement_index(
		pending_replacements,
		maximum_lod
	);
	const ChunkHierarchyIndex retirement_index(
		pending_retirements,
		maximum_lod
	);
	std::vector<WtChunkKey> replacement_queue { seed_replacement };
	std::vector<WtChunkKey> retirement_queue;
	output.replacements.push_back(seed_replacement);
	std::size_t replacement_cursor = 0;
	std::size_t retirement_cursor = 0;
	std::size_t boundary_cursor = 0;
	while (replacement_cursor < replacement_queue.size() ||
		retirement_cursor < retirement_queue.size() ||
		boundary_cursor < output.replacements.size()) {
		while (replacement_cursor < replacement_queue.size()) {
			std::vector<WtChunkKey> overlapping;
			retirement_index.append_overlapping(
				replacement_queue[replacement_cursor++],
				overlapping
			);
			for (const WtChunkKey &retirement : overlapping) {
				if (insert_key(output.retirements, retirement)) {
					retirement_queue.push_back(retirement);
				}
			}
		}
		while (retirement_cursor < retirement_queue.size()) {
			std::vector<WtChunkKey> overlapping;
			replacement_index.append_overlapping(
				retirement_queue[retirement_cursor++],
				overlapping
			);
			for (const WtChunkKey &replacement : overlapping) {
				if (insert_key(output.replacements, replacement)) {
					replacement_queue.push_back(replacement);
				}
			}
		}
		while (boundary_cursor < output.replacements.size()) {
			const WtChunkKey replacement =
				output.replacements[boundary_cursor++];
			for (const WtChunkKey &retirement : pending_retirements) {
				if (unsafe_lod_boundary(replacement, retirement) &&
						insert_key(output.retirements, retirement)) {
					retirement_queue.push_back(retirement);
				}
			}
		}
	}
	return true;
}

bool wt_chunk_publication_region_has_complete_coverage(
	const WtChunkPublicationRegion &region
) noexcept {
	if (region.replacements.empty() || region.retirements.empty()) return false;
	for (std::size_t left = 0; left < region.replacements.size(); ++left) {
		if (!wt_is_valid_chunk_key(region.replacements[left])) return false;
		for (std::size_t right = left + 1;
				right < region.replacements.size();
				++right) {
			if (bounds_overlap(
					region.replacements[left],
					region.replacements[right]
				)) {
				return false;
			}
		}
	}
	for (const WtChunkKey &retirement : region.retirements) {
		if (!wt_is_valid_chunk_key(retirement) ||
				std::find(
					region.replacements.begin(),
					region.replacements.end(),
					retirement
				) != region.replacements.end()) {
			return false;
		}
		if (!replacement_set_covers(retirement, region.replacements)) {
			return false;
		}
	}
	return true;
}

bool wt_collision_retirement_is_safe(
	const WtChunkKey &retirement,
	const std::vector<WtChunkKey> &required_collision_chunks,
	const std::vector<WtChunkKey> &physically_ready_collision_chunks
) noexcept {
	if (!wt_is_valid_chunk_key(retirement)) return false;
	if (!overlaps_any(retirement, required_collision_chunks)) return true;
	return replacement_set_covers(
		retirement,
		physically_ready_collision_chunks
	);
}

} // namespace world_transvoxel
