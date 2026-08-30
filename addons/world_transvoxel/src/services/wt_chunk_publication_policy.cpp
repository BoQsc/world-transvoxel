#include "services/wt_chunk_publication_policy.h"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <set>

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

bool authoritative_replacement_set_covers(
	const WtChunkKey &target,
	const std::vector<WtChunkKey> &replacements,
	const std::function<bool(const WtChunkKey &)> &is_authoritative
) {
	if (!is_authoritative(target)) return true;
	for (const WtChunkKey &replacement : replacements) {
		if (bounds_contains(replacement, target)) return true;
	}
	if (target.lod == 0) return false;
	for (std::int32_t z = 0; z < 2; ++z) {
		for (std::int32_t y = 0; y < 2; ++y) {
			for (std::int32_t x = 0; x < 2; ++x) {
				WtChunkKey child;
				if (!key_child(target, x, y, z, child)) return false;
				if (!is_authoritative(child)) continue;
				if (!overlaps_any(child, replacements) ||
						!authoritative_replacement_set_covers(
							child, replacements, is_authoritative
						)) {
					return false;
				}
			}
		}
	}
	return true;
}

bool valid_non_overlapping_region_replacements(
	const WtChunkPublicationRegion &region
) noexcept {
	if (region.replacements.empty() || region.retirements.empty()) return false;
	for (std::size_t left = 0; left < region.replacements.size(); ++left) {
		if (!wt_is_valid_chunk_key(region.replacements[left])) return false;
		for (std::size_t right = left + 1;
				right < region.replacements.size(); ++right) {
			if (bounds_overlap(
					region.replacements[left], region.replacements[right]
				)) {
				return false;
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

namespace {

void build_indexed_publication_region(
	const WtChunkKey &seed_replacement,
	const std::vector<WtChunkKey> &pending_retirements,
	const ChunkHierarchyIndex &replacement_index,
	const ChunkHierarchyIndex &retirement_index,
	WtChunkPublicationRegion &output
) {
	output = {};
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
}

} // namespace

WtGpuPublicationBoundary wt_gpu_publication_boundary(
	std::uint8_t candidate_mask, bool candidate_mask_known,
	std::uint8_t active_mask, bool active_present
) noexcept {
	// An expectation's zero-initialized mask is not a new boundary. Until a
	// candidate exists, use the complete mask of retained visible geometry.
	const std::uint8_t mask = !candidate_mask_known && active_present ?
		active_mask : candidate_mask;
	return { mask, active_present && mask == active_mask };
}

bool wt_build_chunk_publication_region(
	const WtChunkKey &seed_replacement,
	const std::vector<WtChunkKey> &pending_replacements,
	const std::vector<WtChunkKey> &pending_retirements,
	WtChunkPublicationRegion &output
) {
	output = {};
	if (!wt_is_valid_chunk_key(seed_replacement) || !std::binary_search(
			pending_replacements.begin(), pending_replacements.end(), seed_replacement)) return false;
	std::uint8_t maximum_lod = seed_replacement.lod;
	for (const WtChunkKey &key : pending_replacements) maximum_lod = std::max(maximum_lod, key.lod);
	for (const WtChunkKey &key : pending_retirements) maximum_lod = std::max(maximum_lod, key.lod);
	const ChunkHierarchyIndex replacement_index(pending_replacements, maximum_lod);
	const ChunkHierarchyIndex retirement_index(pending_retirements, maximum_lod);
	build_indexed_publication_region(
		seed_replacement, pending_retirements, replacement_index, retirement_index, output
	);
	return true;
}

bool wt_build_gpu_chunk_publication_cohort(
	const WtChunkKey &seed,
	const std::vector<WtChunkKey> &pending_replacements,
	const std::vector<WtChunkKey> &pending_retirements,
	const std::function<bool(const WtChunkKey &, WtGpuPublicationBoundary &)> &lookup,
	WtChunkPublicationRegion &output,
	std::vector<WtChunkKey> &waiting_masks,
	std::size_t maximum_members
) {
	output = {};
	waiting_masks.clear();
	if (!lookup || !wt_is_valid_chunk_key(seed) || maximum_members == 0) return false;
	std::map<WtChunkKey, WtGpuPublicationBoundary> boundaries;
	std::set<WtChunkKey> absent;
	const auto read = [&](const WtChunkKey &key, WtGpuPublicationBoundary &value) {
		if (!wt_is_valid_chunk_key(key) || std::binary_search(
				pending_retirements.begin(), pending_retirements.end(), key
			) || absent.count(key) != 0) return false;
		const auto existing = boundaries.find(key);
		if (existing != boundaries.end()) {
			value = existing->second;
			return true;
		}
		if (!lookup(key, value)) {
			absent.insert(key);
			return false;
		}
		boundaries.emplace(key, value);
		return true;
	};
	WtGpuPublicationBoundary seed_boundary;
	if (!read(seed, seed_boundary)) return false;
	const auto key_at = [](std::int64_t x, std::int64_t y, std::int64_t z,
			std::uint8_t lod, WtChunkKey &key) {
		const auto minimum = std::numeric_limits<std::int32_t>::min();
		const auto maximum = std::numeric_limits<std::int32_t>::max();
		if (x < minimum || x > maximum || y < minimum || y > maximum ||
			z < minimum || z > maximum) return false;
		key = { static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
			static_cast<std::int32_t>(z), lod };
		return wt_is_valid_chunk_key(key);
	};
	std::set<WtChunkKey> selected;
	std::set<WtChunkKey> expanded_regions;
	std::uint8_t indexed_maximum_lod = seed.lod;
	for (const WtChunkKey &key : pending_replacements) indexed_maximum_lod = std::max(indexed_maximum_lod, key.lod);
	for (const WtChunkKey &key : pending_retirements) indexed_maximum_lod = std::max(indexed_maximum_lod, key.lod);
	std::unique_ptr<ChunkHierarchyIndex> replacement_index;
	std::unique_ptr<ChunkHierarchyIndex> retirement_index;
	std::vector<WtChunkKey> queue;
	const auto add = [&](const WtChunkKey &key) {
		if (selected.count(key) != 0) return true;
		if (selected.size() >= maximum_members) return false;
		selected.insert(key);
		queue.push_back(key);
		return true;
	};
	add(seed);
	for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
		const WtChunkKey key = queue[cursor];
		WtGpuPublicationBoundary boundary;
		if (!read(key, boundary)) continue; // Readiness validation rejects missing members.
		// Compatible active geometry already satisfies this boundary; content
		// generation replacement can proceed without extending the cohort.
		if (boundary.compatible_active) continue;
		// Any newly required boundary member may itself replace retained chunks.
		// Include that overlap component and its retirements in the same swap.
		if (expanded_regions.count(key) == 0 &&
			wt_chunk_replacement_requires_regional_publication(key, pending_retirements)) {
			if (!std::binary_search(pending_replacements.begin(), pending_replacements.end(), key)) return false;
			if (!replacement_index || key.lod > indexed_maximum_lod) {
				indexed_maximum_lod = std::max(indexed_maximum_lod, key.lod);
				replacement_index = std::make_unique<ChunkHierarchyIndex>(pending_replacements, indexed_maximum_lod);
				retirement_index = std::make_unique<ChunkHierarchyIndex>(pending_retirements, indexed_maximum_lod);
			}
			WtChunkPublicationRegion region;
			build_indexed_publication_region(
				key, pending_retirements, *replacement_index, *retirement_index, region
			);
			for (const WtChunkKey &replacement : region.replacements) {
				if (!add(replacement)) return false;
				expanded_regions.insert(replacement);
			}
			for (const WtChunkKey &retirement : region.retirements) {
				insert_key(output.retirements, retirement);
			}
			if (output.retirements.size() > maximum_members) return false;
		}
		for (std::uint8_t face_index = 0; face_index < 6; ++face_index) {
			const auto face = static_cast<WtChunkFace>(face_index);
			const auto opposite = wt_opposite_face(face);
			const auto bit = wt_face_bit(face);
			const auto opposite_bit = wt_face_bit(opposite);
			const std::size_t axis = face_index / 2;
			const std::int64_t sign = (face_index & 1U) == 0 ? -1 : 1;
			std::int64_t coordinate[3] { key.x, key.y, key.z };
			coordinate[axis] += sign;
			WtChunkKey adjacent;
			WtGpuPublicationBoundary neighbor;
			if (key_at(coordinate[0], coordinate[1], coordinate[2], key.lod, adjacent) &&
				read(adjacent, neighbor)) {
				if ((boundary.transition_mask & bit) != 0) insert_key(waiting_masks, key);
				if ((neighbor.transition_mask & opposite_bit) != 0) {
					insert_key(waiting_masks, adjacent);
				}
				// Compatible active geometry creates no additional dependency.
				if (!neighbor.compatible_active ||
						(neighbor.transition_mask & opposite_bit) != 0) {
					if (!add(adjacent)) return false;
				}
				continue;
			}
			if (key.lod < kWtMaximumLod) {
				const WtChunkKey parent = wt_parent_chunk_key(key);
				const std::int64_t parent_coordinate[3] { parent.x, parent.y, parent.z };
				const std::int64_t own_coordinate[3] { key.x, key.y, key.z };
				const std::int64_t parity = own_coordinate[axis] - 2 * parent_coordinate[axis];
				if (parity == ((sign < 0) ? 0 : 1)) {
					coordinate[0] = parent.x; coordinate[1] = parent.y; coordinate[2] = parent.z;
					coordinate[axis] += sign;
					if (key_at(coordinate[0], coordinate[1], coordinate[2], key.lod + 1, adjacent) &&
						read(adjacent, neighbor)) {
						if ((boundary.transition_mask & bit) != 0) insert_key(waiting_masks, key);
						if ((neighbor.transition_mask & opposite_bit) == 0) {
							insert_key(waiting_masks, adjacent);
						}
						if (!neighbor.compatible_active ||
								(neighbor.transition_mask & opposite_bit) == 0) {
							if (!add(adjacent)) return false;
						}
						continue;
					}
				}
			}
			if (key.lod == 0) continue;
			std::vector<WtChunkKey> fine_keys;
			std::size_t fine_present = 0;
			bool fine_coordinates_valid = true;
			for (std::int64_t first = 0; first < 2; ++first) {
				for (std::int64_t second = 0; second < 2; ++second) {
					coordinate[0] = 2LL * key.x;
					coordinate[1] = 2LL * key.y;
					coordinate[2] = 2LL * key.z;
					coordinate[axis] += sign < 0 ? -1 : 2;
					coordinate[(axis + 1) % 3] += first;
					coordinate[(axis + 2) % 3] += second;
					WtChunkKey fine;
					if (!key_at(coordinate[0], coordinate[1], coordinate[2], key.lod - 1, fine)) {
						fine_coordinates_valid = false;
						continue;
					}
					fine_keys.push_back(fine);
					if (read(fine, neighbor)) {
						++fine_present;
						if ((neighbor.transition_mask & opposite_bit) != 0) {
							insert_key(waiting_masks, fine);
						}
					}
				}
			}
			if (fine_present != 0 || (boundary.transition_mask & bit) != 0) {
				if (!fine_coordinates_valid) return false;
				if ((boundary.transition_mask & bit) == 0) insert_key(waiting_masks, key);
				for (const WtChunkKey &fine : fine_keys) {
					WtGpuPublicationBoundary fine_boundary;
					if (!read(fine, fine_boundary) || !fine_boundary.compatible_active ||
							(fine_boundary.transition_mask & opposite_bit) != 0) {
						if (!add(fine)) return false;
					}
				}
			}
		}
	}
	output.replacements.assign(selected.begin(), selected.end());
	return true;
}

bool wt_chunk_publication_region_has_complete_coverage(
	const WtChunkPublicationRegion &region
) noexcept {
	if (!valid_non_overlapping_region_replacements(region)) return false;
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

bool wt_chunk_publication_region_has_complete_authoritative_coverage(
	const WtChunkPublicationRegion &region,
	const std::function<bool(const WtChunkKey &)> &is_authoritative
) {
	if (!is_authoritative ||
			!valid_non_overlapping_region_replacements(region)) {
		return false;
	}
	for (const WtChunkKey &replacement : region.replacements) {
		if (!is_authoritative(replacement)) return false;
	}
	for (const WtChunkKey &retirement : region.retirements) {
		if (!wt_is_valid_chunk_key(retirement) || !is_authoritative(retirement) ||
				std::find(
					region.replacements.begin(),
					region.replacements.end(),
					retirement
				) != region.replacements.end() ||
				!authoritative_replacement_set_covers(
					retirement, region.replacements, is_authoritative
				)) {
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

bool wt_required_collision_can_publish_independently(
	WtGenerationToken record_generation,
	WtGenerationToken render_generation,
	WtGenerationToken collision_generation,
	WtGenerationToken staged_collision_generation,
	bool collision_required,
	bool collision_ready,
	bool visual_required
) noexcept {
	if (!collision_required || !collision_ready ||
			record_generation.value == 0 || collision_generation.value != 0 ||
			staged_collision_generation != record_generation) {
		return false;
	}
	// A new physical support shape may join retained coarse collision once its
	// matching visual is live. Existing same-key shapes remain synchronized with
	// their staged render replacement.
	return !visual_required || render_generation == record_generation;
}

} // namespace world_transvoxel
