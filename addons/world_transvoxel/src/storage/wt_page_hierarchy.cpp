#include "storage/wt_page_hierarchy.h"

#include "storage/wt_procedural_world_source.h"
#include "storage/wt_world_manifest.h"

#include <algorithm>
#include <limits>

namespace world_transvoxel {
namespace {

std::array<WtChunkKey, 8> child_keys(const WtChunkKey &parent) noexcept {
	std::array<WtChunkKey, 8> children{};
	std::size_t index = 0;
	for (std::int32_t z = 0; z < 2; ++z) {
		for (std::int32_t y = 0; y < 2; ++y) {
			for (std::int32_t x = 0; x < 2; ++x) {
				children[index++] = {
					static_cast<std::int32_t>(parent.x * 2 + x),
					static_cast<std::int32_t>(parent.y * 2 + y),
					static_cast<std::int32_t>(parent.z * 2 + z),
					static_cast<std::uint8_t>(parent.lod - 1),
				};
			}
		}
	}
	return children;
}

bool in_range(
	const WtChunkKey &key,
	const WtChunkKey &minimum,
	const WtChunkKey &maximum
) noexcept {
	return key.lod == minimum.lod && key.lod == maximum.lod &&
		key.x >= minimum.x && key.x <= maximum.x &&
		key.y >= minimum.y && key.y <= maximum.y &&
		key.z >= minimum.z && key.z <= maximum.z;
}

} // namespace

WtPageHierarchy WtPageHierarchy::explicit_catalog(
	std::vector<WtChunkKey> keys
) {
	WtPageHierarchy hierarchy;
	hierarchy.kind_ = WtPageHierarchyKind::ExplicitCatalog;
	std::sort(keys.begin(), keys.end());
	hierarchy.valid_ = keys.size() <= kWtMaximumWorldPageCount;
	for (std::size_t index = 0; hierarchy.valid_ && index < keys.size(); ++index) {
		const WtChunkKey key = keys[index];
		const bool refinable_coordinates = key.lod == 0 ||
			(key.x >= std::numeric_limits<std::int32_t>::min() / 2 &&
				key.x <= std::numeric_limits<std::int32_t>::max() / 2 &&
				key.y >= std::numeric_limits<std::int32_t>::min() / 2 &&
				key.y <= std::numeric_limits<std::int32_t>::max() / 2 &&
				key.z >= std::numeric_limits<std::int32_t>::min() / 2 &&
				key.z <= std::numeric_limits<std::int32_t>::max() / 2);
		hierarchy.valid_ = wt_is_valid_chunk_key(key) && refinable_coordinates &&
			(index == 0 || keys[index - 1] < keys[index]);
		if (hierarchy.valid_) {
			++hierarchy.lod_counts_[key.lod];
		}
	}
	hierarchy.explicit_keys_ = std::move(keys);
	hierarchy.metrics_.kind = hierarchy.kind_;
	hierarchy.metrics_.declared_page_count = hierarchy.explicit_keys_.size();
	hierarchy.metrics_.explicit_index_entries = hierarchy.explicit_keys_.size();
	hierarchy.metrics_.estimated_index_bytes =
		hierarchy.explicit_keys_.capacity() * sizeof(WtChunkKey);
	return hierarchy;
}

WtPageHierarchy WtPageHierarchy::implicit_procedural(
	const WtProceduralWorldDescriptor &descriptor
) {
	WtPageHierarchy hierarchy;
	hierarchy.kind_ = WtPageHierarchyKind::ImplicitProcedural;
	hierarchy.procedural_descriptor_ = descriptor;
	hierarchy.valid_ = wt_valid_procedural_descriptor(descriptor);
	if (hierarchy.valid_) {
		for (std::uint8_t lod = 0; lod <= kWtProceduralMaximumLod; ++lod) {
			hierarchy.lod_counts_[lod] = static_cast<std::size_t>(
				wt_procedural_lod_page_count(descriptor, lod)
			);
		}
	}
	hierarchy.metrics_.kind = hierarchy.kind_;
	hierarchy.metrics_.declared_page_count = wt_procedural_page_count(descriptor);
	hierarchy.metrics_.estimated_index_bytes = sizeof(WtProceduralWorldDescriptor);
	return hierarchy;
}

bool WtPageHierarchy::valid() const noexcept {
	return valid_;
}

WtPageHierarchyKind WtPageHierarchy::kind() const noexcept {
	return kind_;
}

std::size_t WtPageHierarchy::page_count() const noexcept {
	return valid_ ? static_cast<std::size_t>(metrics_.declared_page_count) : 0;
}

std::size_t WtPageHierarchy::lod_page_count(std::uint8_t lod) const noexcept {
	return valid_ && lod <= kWtMaximumLod ? lod_counts_[lod] : 0;
}

bool WtPageHierarchy::contains(const WtChunkKey &key) const noexcept {
	++metrics_.membership_queries;
	if (!valid_) return false;
	return kind_ == WtPageHierarchyKind::ImplicitProcedural ?
		wt_procedural_is_declared_page(procedural_descriptor_, key) :
		std::binary_search(explicit_keys_.begin(), explicit_keys_.end(), key);
}

bool WtPageHierarchy::complete_children(
	const WtChunkKey &parent,
	std::array<WtChunkKey, 8> &children
) const noexcept {
	++metrics_.child_queries;
	if (!valid_ || parent.lod == 0 || !contains(parent)) return false;
	children = child_keys(parent);
	return std::all_of(
		children.begin(), children.end(),
		[&](const WtChunkKey &child) { return contains(child); }
	);
}

bool WtPageHierarchy::ancestor(
	const WtChunkKey &key,
	std::uint8_t ancestor_lod,
	WtChunkKey &output
) const noexcept {
	++metrics_.ancestor_queries;
	output = {};
	if (!valid_ || ancestor_lod < key.lod || ancestor_lod > kWtMaximumLod ||
		!contains(key)) return false;
	output = key;
	while (output.lod < ancestor_lod) output = wt_parent_chunk_key(output);
	return contains(output);
}

bool WtPageHierarchy::append_face_neighbors(
	const WtChunkKey &key,
	std::vector<WtChunkKey> &output,
	std::size_t capacity
) const {
	++metrics_.neighbor_queries;
	output.clear();
	if (!valid_ || !contains(key)) return false;
	constexpr std::array<std::array<std::int64_t, 3>, 6> offsets = {{
		{{ -1, 0, 0 }}, {{ 1, 0, 0 }},
		{{ 0, -1, 0 }}, {{ 0, 1, 0 }},
		{{ 0, 0, -1 }}, {{ 0, 0, 1 }},
	}};
	for (const auto &offset : offsets) {
		const std::int64_t x = static_cast<std::int64_t>(key.x) + offset[0];
		const std::int64_t y = static_cast<std::int64_t>(key.y) + offset[1];
		const std::int64_t z = static_cast<std::int64_t>(key.z) + offset[2];
		if (x < std::numeric_limits<std::int32_t>::min() ||
			x > std::numeric_limits<std::int32_t>::max() ||
			y < std::numeric_limits<std::int32_t>::min() ||
			y > std::numeric_limits<std::int32_t>::max() ||
			z < std::numeric_limits<std::int32_t>::min() ||
			z > std::numeric_limits<std::int32_t>::max()) continue;
		const WtChunkKey neighbor {
			static_cast<std::int32_t>(x),
			static_cast<std::int32_t>(y),
			static_cast<std::int32_t>(z),
			key.lod,
		};
		if (contains(neighbor)) {
			if (output.size() >= capacity) return false;
			output.push_back(neighbor);
		}
	}
	return true;
}

bool WtPageHierarchy::append_lod_keys(
	std::uint8_t lod,
	std::vector<WtChunkKey> &output,
	std::size_t capacity
) const {
	++metrics_.lod_enumerations;
	const std::size_t count = lod_page_count(lod);
	if (!valid_ || output.size() > capacity || count > capacity - output.size()) {
		return false;
	}
	if (kind_ == WtPageHierarchyKind::ImplicitProcedural) {
		return wt_append_procedural_lod_keys(
			procedural_descriptor_, lod, output, capacity
		);
	}
	for (const WtChunkKey &key : explicit_keys_) {
		if (key.lod == lod) output.push_back(key);
	}
	return true;
}

bool WtPageHierarchy::query_range(
	const WtChunkKey &minimum,
	const WtChunkKey &maximum,
	std::vector<WtChunkKey> &output,
	std::size_t capacity
) const {
	++metrics_.range_queries;
	output.clear();
	if (!valid_ || minimum.lod != maximum.lod ||
		minimum.x > maximum.x || minimum.y > maximum.y ||
		minimum.z > maximum.z || output.size() > capacity) {
		return false;
	}
	if (kind_ == WtPageHierarchyKind::ExplicitCatalog) {
		for (const WtChunkKey &key : explicit_keys_) {
			if (in_range(key, minimum, maximum)) {
				if (output.size() >= capacity) return false;
				output.push_back(key);
			}
		}
		return true;
	}
	WtChunkKey declared_minimum;
	WtChunkKey declared_maximum;
	if (!wt_procedural_lod_key_bounds(
			procedural_descriptor_, minimum.lod,
			declared_minimum, declared_maximum
		)) {
		return true;
	}
	const std::int64_t min_x = std::max(minimum.x, declared_minimum.x);
	const std::int64_t min_y = std::max(minimum.y, declared_minimum.y);
	const std::int64_t min_z = std::max(minimum.z, declared_minimum.z);
	const std::int64_t max_x = std::min(maximum.x, declared_maximum.x);
	const std::int64_t max_y = std::min(maximum.y, declared_maximum.y);
	const std::int64_t max_z = std::min(maximum.z, declared_maximum.z);
	for (std::int64_t z = min_z; z <= max_z; ++z) {
		for (std::int64_t y = min_y; y <= max_y; ++y) {
			for (std::int64_t x = min_x; x <= max_x; ++x) {
				const WtChunkKey key {
					static_cast<std::int32_t>(x),
					static_cast<std::int32_t>(y),
					static_cast<std::int32_t>(z),
					minimum.lod,
				};
				if (!wt_procedural_is_declared_page(procedural_descriptor_, key)) {
					continue;
				}
				if (output.size() >= capacity) return false;
				output.push_back(key);
			}
		}
	}
	std::sort(output.begin(), output.end());
	return true;
}

bool WtPageHierarchy::query_viewer_roots(
	const WtChunkKey &center,
	std::uint32_t radius,
	std::vector<WtChunkKey> &output,
	std::size_t capacity
) const {
	++metrics_.viewer_root_queries;
	const std::int64_t signed_radius = radius;
	const auto clamp_coordinate = [](std::int64_t value) {
		return static_cast<std::int32_t>(std::clamp<std::int64_t>(
			value,
			std::numeric_limits<std::int32_t>::min(),
			std::numeric_limits<std::int32_t>::max()
		));
	};
	const WtChunkKey minimum {
		clamp_coordinate(static_cast<std::int64_t>(center.x) - signed_radius),
		clamp_coordinate(static_cast<std::int64_t>(center.y) - signed_radius),
		clamp_coordinate(static_cast<std::int64_t>(center.z) - signed_radius),
		center.lod,
	};
	const WtChunkKey maximum {
		clamp_coordinate(static_cast<std::int64_t>(center.x) + signed_radius),
		clamp_coordinate(static_cast<std::int64_t>(center.y) + signed_radius),
		clamp_coordinate(static_cast<std::int64_t>(center.z) + signed_radius),
		center.lod,
	};
	return query_range(minimum, maximum, output, capacity);
}

WtPageHierarchyMetrics WtPageHierarchy::metrics() const noexcept {
	return metrics_;
}

const char *wt_page_hierarchy_kind_name(WtPageHierarchyKind kind) noexcept {
	switch (kind) {
		case WtPageHierarchyKind::Invalid: return "invalid";
		case WtPageHierarchyKind::ExplicitCatalog: return "explicit_catalog";
		case WtPageHierarchyKind::ImplicitProcedural: return "implicit_procedural";
	}
	return "invalid";
}

} // namespace world_transvoxel
