#pragma once

#include "core/wt_chunk_key.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

// Immutable, balanced AABB hierarchy for publication dependencies. All bounds
// and face tests remain integer grid coordinates, including at LOD20 limits.
class WtPublicationSpatialIndex {
public:
	struct QueryStats {
		std::size_t visited_nodes = 0;
		std::size_t tested_keys = 0;
	};

	explicit WtPublicationSpatialIndex(const std::vector<WtChunkKey> &keys) {
		entries_.reserve(keys.size());
		for (const auto &key : keys) {
			if (wt_is_valid_chunk_key(key)) entries_.push_back({key, wt_chunk_bounds(key)});
		}
		if (!entries_.empty()) {
			nodes_.reserve(entries_.size() * 2);
			build(0, entries_.size());
		}
	}

	void append_dependencies(const WtChunkKey &key, bool include_unsafe_faces,
		std::vector<WtChunkKey> &output, QueryStats *stats = nullptr) const {
		if (nodes_.empty() || !wt_is_valid_chunk_key(key)) return;
		query(0, key, wt_chunk_bounds(key), include_unsafe_faces, output, stats);
	}

	bool overlaps(const WtChunkKey &key) const {
		if (nodes_.empty() || !wt_is_valid_chunk_key(key)) return false;
		return any_overlap(0, wt_chunk_bounds(key));
	}

private:
	struct Entry { WtChunkKey key; WtChunkBounds bounds; };
	struct Node {
		WtChunkBounds bounds;
		std::size_t begin = 0, end = 0, left = 0, right = 0;
		bool leaf() const { return end - begin <= 4; }
	};
	std::vector<Entry> entries_;
	std::vector<Node> nodes_;

	static std::int64_t axis(const WtGridPoint &p, unsigned dimension) {
		return dimension == 0 ? p.x : dimension == 1 ? p.y : p.z;
	}
	static bool intersects(const WtChunkBounds &a, const WtChunkBounds &b, bool touching) {
		for (unsigned d = 0; d < 3; ++d) {
			if (touching ? axis(a.maximum, d) < axis(b.minimum, d) || axis(b.maximum, d) < axis(a.minimum, d)
				: axis(a.maximum, d) <= axis(b.minimum, d) || axis(b.maximum, d) <= axis(a.minimum, d)) return false;
		}
		return true;
	}
	static bool dependency(const WtChunkKey &key, const WtChunkBounds &bounds,
		const Entry &entry, bool include_unsafe_faces) {
		unsigned overlaps = 0, touches = 0;
		for (unsigned d = 0; d < 3; ++d) {
			overlaps += axis(bounds.minimum, d) < axis(entry.bounds.maximum, d) &&
				axis(entry.bounds.minimum, d) < axis(bounds.maximum, d);
			touches += axis(bounds.minimum, d) == axis(entry.bounds.maximum, d) ||
				axis(entry.bounds.minimum, d) == axis(bounds.maximum, d);
		}
		const int gap = int(key.lod) - int(entry.key.lod);
		return overlaps == 3 || (include_unsafe_faces && overlaps == 2 && touches == 1 &&
			(gap > 1 || gap < -1));
	}
	std::size_t build(std::size_t begin, std::size_t end) {
		WtChunkBounds bounds = entries_[begin].bounds;
		for (std::size_t i = begin + 1; i < end; ++i) {
			const auto &b = entries_[i].bounds;
			bounds.minimum = {std::min(bounds.minimum.x, b.minimum.x), std::min(bounds.minimum.y, b.minimum.y), std::min(bounds.minimum.z, b.minimum.z)};
			bounds.maximum = {std::max(bounds.maximum.x, b.maximum.x), std::max(bounds.maximum.y, b.maximum.y), std::max(bounds.maximum.z, b.maximum.z)};
		}
		const auto index = nodes_.size();
		nodes_.push_back({bounds, begin, end, 0, 0});
		if (end - begin <= 4) return index;
		unsigned dimension = 0;
		for (unsigned d = 1; d < 3; ++d) {
			if (axis(bounds.maximum, d) - axis(bounds.minimum, d) >
				axis(bounds.maximum, dimension) - axis(bounds.minimum, dimension)) dimension = d;
		}
		const auto middle = begin + (end - begin) / 2;
		std::nth_element(entries_.begin() + begin, entries_.begin() + middle, entries_.begin() + end,
			[dimension](const Entry &a, const Entry &b) {
				const auto ca = axis(a.bounds.minimum, dimension) + axis(a.bounds.maximum, dimension);
				const auto cb = axis(b.bounds.minimum, dimension) + axis(b.bounds.maximum, dimension);
				return ca != cb ? ca < cb : a.key < b.key;
			});
		const auto left = build(begin, middle);
		const auto right = build(middle, end);
		nodes_[index].left = left;
		nodes_[index].right = right;
		return index;
	}
	void query(std::size_t index, const WtChunkKey &key, const WtChunkBounds &bounds,
		bool include_unsafe_faces, std::vector<WtChunkKey> &output, QueryStats *stats) const {
		const auto &node = nodes_[index];
		if (stats) ++stats->visited_nodes;
		if (!intersects(bounds, node.bounds, include_unsafe_faces)) return;
		if (node.leaf()) {
			for (auto i = node.begin; i < node.end; ++i) {
				if (stats) ++stats->tested_keys;
				if (dependency(key, bounds, entries_[i], include_unsafe_faces)) output.push_back(entries_[i].key);
			}
			return;
		}
		query(node.left, key, bounds, include_unsafe_faces, output, stats);
		query(node.right, key, bounds, include_unsafe_faces, output, stats);
	}
	bool any_overlap(std::size_t index, const WtChunkBounds &bounds) const {
		const auto &node = nodes_[index];
		if (!intersects(bounds, node.bounds, false)) return false;
		if (node.leaf()) {
			for (auto i = node.begin; i < node.end; ++i) {
				if (intersects(bounds, entries_[i].bounds, false)) return true;
			}
			return false;
		}
		return any_overlap(node.left, bounds) || any_overlap(node.right, bounds);
	}
};

} // namespace world_transvoxel
