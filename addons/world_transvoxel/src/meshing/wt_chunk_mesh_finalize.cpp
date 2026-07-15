#include "meshing/wt_chunk_mesh_finalize.h"

#include "meshing/wt_chunk_mesh_geometry.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <vector>

namespace world_transvoxel {
namespace {

constexpr double kMinimumTriangleEdgeLengthSquared = 0.000001;
constexpr double kMinimumTriangleThinRatioSquared = 1.0e-12;
constexpr double kMinimumNormalAlignmentMagnitude = 0.000000001;
constexpr double kPositionKeyScale = 1024.0;

struct PositionKey {
	std::int64_t x = 0;
	std::int64_t y = 0;
	std::int64_t z = 0;

	bool operator==(const PositionKey &other) const noexcept {
		return x == other.x && y == other.y && z == other.z;
	}
};

bool operator<(const PositionKey &left, const PositionKey &right) noexcept {
	if (left.x != right.x) return left.x < right.x;
	if (left.y != right.y) return left.y < right.y;
	return left.z < right.z;
}

struct PositionKeyHash {
	std::size_t operator()(const PositionKey &key) const noexcept {
		std::size_t hash = static_cast<std::size_t>(key.x);
		hash ^= static_cast<std::size_t>(key.y) + 0x9e3779b9U +
			(hash << 6U) + (hash >> 2U);
		hash ^= static_cast<std::size_t>(key.z) + 0x9e3779b9U +
			(hash << 6U) + (hash >> 2U);
		return hash;
	}
};

PositionKey make_position_key(const WtVec3 &position) noexcept {
	return {
		static_cast<std::int64_t>(std::llround(
			static_cast<double>(position.x) * kPositionKeyScale
		)),
		static_cast<std::int64_t>(std::llround(
			static_cast<double>(position.y) * kPositionKeyScale
		)),
		static_cast<std::int64_t>(std::llround(
			static_cast<double>(position.z) * kPositionKeyScale
		)),
	};
}

struct EdgeKey {
	PositionKey first;
	PositionKey second;

	bool operator==(const EdgeKey &other) const noexcept {
		return first == other.first && second == other.second;
	}
};

struct EdgeKeyHash {
	std::size_t operator()(const EdgeKey &key) const noexcept {
		PositionKeyHash position_hash;
		std::size_t hash = position_hash(key.first);
		hash ^= position_hash(key.second) + 0x9e3779b9U +
			(hash << 6U) + (hash >> 2U);
		return hash;
	}
};

struct OrientedEdge {
	EdgeKey key;
	bool forward = true;
};

OrientedEdge make_oriented_edge(
	const WtVec3 &a,
	const WtVec3 &b
) noexcept {
	const PositionKey key_a = make_position_key(a);
	const PositionKey key_b = make_position_key(b);
	if (key_b < key_a) {
		return { { key_b, key_a }, false };
	}
	return { { key_a, key_b }, true };
}

struct TriangleRecord {
	std::uint32_t a = 0;
	std::uint32_t b = 0;
	std::uint32_t c = 0;
	bool flip = false;
	bool visited = false;
};

struct EdgeIncident {
	std::size_t triangle = 0;
	bool forward = true;
};

std::uint32_t triangle_index_at(
	const TriangleRecord &triangle,
	unsigned int index
) noexcept {
	if (index == 0U) {
		return triangle.a;
	}
	if (triangle.flip) {
		return index == 1U ? triangle.c : triangle.b;
	}
	return index == 1U ? triangle.b : triangle.c;
}

bool triangle_is_valid(
	const WtChunkMeshBuffer &buffer,
	std::uint32_t index_a,
	std::uint32_t index_b,
	std::uint32_t index_c
) noexcept {
	if (index_a >= buffer.vertices.size() ||
		index_b >= buffer.vertices.size() ||
		index_c >= buffer.vertices.size()) {
		return false;
	}
	const WtVec3 &a = buffer.vertices[index_a].position;
	const WtVec3 &b = buffer.vertices[index_b].position;
	const WtVec3 &c = buffer.vertices[index_c].position;
	const double ab_x = static_cast<double>(b.x) - a.x;
	const double ab_y = static_cast<double>(b.y) - a.y;
	const double ab_z = static_cast<double>(b.z) - a.z;
	const double ac_x = static_cast<double>(c.x) - a.x;
	const double ac_y = static_cast<double>(c.y) - a.y;
	const double ac_z = static_cast<double>(c.z) - a.z;
	const double bc_x = static_cast<double>(c.x) - b.x;
	const double bc_y = static_cast<double>(c.y) - b.y;
	const double bc_z = static_cast<double>(c.z) - b.z;
	const double ab_length_squared = ab_x * ab_x + ab_y * ab_y + ab_z * ab_z;
	const double ac_length_squared = ac_x * ac_x + ac_y * ac_y + ac_z * ac_z;
	const double bc_length_squared = bc_x * bc_x + bc_y * bc_y + bc_z * bc_z;
	if (ab_length_squared <= kMinimumTriangleEdgeLengthSquared ||
		ac_length_squared <= kMinimumTriangleEdgeLengthSquared ||
		bc_length_squared <= kMinimumTriangleEdgeLengthSquared) {
		return false;
	}
	const double cross_x = ab_y * ac_z - ab_z * ac_y;
	const double cross_y = ab_z * ac_x - ab_x * ac_z;
	const double cross_z = ab_x * ac_y - ab_y * ac_x;
	const double area_squared =
		cross_x * cross_x + cross_y * cross_y + cross_z * cross_z;
	const double maximum_edge_squared =
		std::max({ ab_length_squared, ac_length_squared, bc_length_squared });
	return area_squared > maximum_edge_squared * maximum_edge_squared *
		kMinimumTriangleThinRatioSquared;
}

void add_edge_incident(
	const WtChunkMeshBuffer &buffer,
	const std::vector<TriangleRecord> &triangles,
	std::size_t triangle_index,
	unsigned int edge_index,
	std::unordered_map<EdgeKey, std::vector<EdgeIncident>, EdgeKeyHash> &edges
) {
	const TriangleRecord &triangle = triangles[triangle_index];
	const std::uint32_t indices[3] = { triangle.a, triangle.b, triangle.c };
	const WtVec3 &a = buffer.vertices[indices[edge_index]].position;
	const WtVec3 &b = buffer.vertices[indices[(edge_index + 1U) % 3U]].position;
	const OrientedEdge edge = make_oriented_edge(a, b);
	edges[edge.key].push_back({ triangle_index, edge.forward });
}

double normal_alignment_score(
	const WtChunkMeshBuffer &buffer,
	const TriangleRecord &triangle
) noexcept {
	const WtCellVertex &vertex_a =
		buffer.vertices[triangle_index_at(triangle, 0U)];
	const WtCellVertex &vertex_b =
		buffer.vertices[triangle_index_at(triangle, 1U)];
	const WtCellVertex &vertex_c =
		buffer.vertices[triangle_index_at(triangle, 2U)];
	const WtVec3 &a = vertex_a.position;
	const WtVec3 &b = vertex_b.position;
	const WtVec3 &c = vertex_c.position;
	const double ab_x = static_cast<double>(b.x) - a.x;
	const double ab_y = static_cast<double>(b.y) - a.y;
	const double ab_z = static_cast<double>(b.z) - a.z;
	const double ac_x = static_cast<double>(c.x) - a.x;
	const double ac_y = static_cast<double>(c.y) - a.y;
	const double ac_z = static_cast<double>(c.z) - a.z;
	const double cross_x = ab_y * ac_z - ab_z * ac_y;
	const double cross_y = ab_z * ac_x - ab_x * ac_z;
	const double cross_z = ab_x * ac_y - ab_y * ac_x;
	const double normal_x =
		static_cast<double>(vertex_a.normal.x) +
		static_cast<double>(vertex_b.normal.x) +
		static_cast<double>(vertex_c.normal.x);
	const double normal_y =
		static_cast<double>(vertex_a.normal.y) +
		static_cast<double>(vertex_b.normal.y) +
		static_cast<double>(vertex_c.normal.y);
	const double normal_z =
		static_cast<double>(vertex_a.normal.z) +
		static_cast<double>(vertex_b.normal.z) +
		static_cast<double>(vertex_c.normal.z);
	const double cross_length_squared =
		cross_x * cross_x + cross_y * cross_y + cross_z * cross_z;
	const double normal_length_squared =
		normal_x * normal_x + normal_y * normal_y + normal_z * normal_z;
	if (cross_length_squared == 0.0 || normal_length_squared == 0.0) {
		return 0.0;
	}
	const double score =
		cross_x * normal_x + cross_y * normal_y + cross_z * normal_z;
	return std::isfinite(score) ? score : 0.0;
}

void orient_component_to_normals(
	const WtChunkMeshBuffer &buffer,
	std::vector<TriangleRecord> &triangles,
	const std::vector<std::size_t> &component
) {
	double score = 0.0;
	double magnitude = 0.0;
	for (const std::size_t triangle_index : component) {
		const double alignment =
			normal_alignment_score(buffer, triangles[triangle_index]);
		score += alignment;
		magnitude += std::fabs(alignment);
	}
	if (magnitude <= kMinimumNormalAlignmentMagnitude) {
		return;
	}
	if (score < -kMinimumNormalAlignmentMagnitude) {
		for (const std::size_t triangle_index : component) {
			triangles[triangle_index].flip = !triangles[triangle_index].flip;
		}
	}
}

void orient_consistent_components(
	const WtChunkMeshBuffer &buffer,
	std::vector<TriangleRecord> &triangles,
	const std::unordered_map<EdgeKey, std::vector<EdgeIncident>, EdgeKeyHash> &edges
) {
	for (std::size_t start = 0; start < triangles.size(); ++start) {
		if (triangles[start].visited) {
			continue;
		}
		std::deque<std::size_t> queue;
		std::vector<std::size_t> component;
		triangles[start].visited = true;
		triangles[start].flip = false;
		queue.push_back(start);
		component.push_back(start);
		while (!queue.empty()) {
			const std::size_t current = queue.front();
			queue.pop_front();
			const TriangleRecord &triangle = triangles[current];
			const std::uint32_t indices[3] = { triangle.a, triangle.b, triangle.c };
			for (unsigned int edge_index = 0; edge_index < 3U; ++edge_index) {
				const OrientedEdge edge = make_oriented_edge(
					buffer.vertices[indices[edge_index]].position,
					buffer.vertices[indices[(edge_index + 1U) % 3U]].position
				);
				const auto found = edges.find(edge.key);
				if (found == edges.end() || found->second.size() != 2U) {
					continue;
				}
				const EdgeIncident &first = found->second[0];
				const EdgeIncident &second = found->second[1];
				const EdgeIncident &self = first.triangle == current ? first : second;
				const EdgeIncident &other = first.triangle == current ? second : first;
				if (other.triangle == current) {
					continue;
				}
				const bool same_direction = self.forward == other.forward;
				const bool required_neighbor_flip =
					triangles[current].flip ^ same_direction;
				if (!triangles[other.triangle].visited) {
					triangles[other.triangle].visited = true;
					triangles[other.triangle].flip = required_neighbor_flip;
					queue.push_back(other.triangle);
					component.push_back(other.triangle);
				}
			}
		}
		// Edge propagation gives each connected component internally consistent
		// winding. The component still needs a global outward/inward decision:
		// lookup tables and edits can produce a consistently inverted island.
		// Use interpolated SDF normals only as a whole-component anchor so shared
		// edges remain consistently oriented after any flip.
		orient_component_to_normals(buffer, triangles, component);
	}
}

void compact_unreferenced_vertices(WtChunkMeshBuffer &buffer) {
	std::vector<bool> referenced(buffer.vertices.size(), false);
	for (std::uint32_t index : buffer.indices) {
		referenced[index] = true;
	}
	std::vector<std::uint32_t> remap(buffer.vertices.size(), 0);
	std::vector<WtCellVertex> compacted;
	compacted.reserve(buffer.vertices.size());
	for (std::size_t index = 0; index < buffer.vertices.size(); ++index) {
		if (!referenced[index]) {
			continue;
		}
		remap[index] = static_cast<std::uint32_t>(compacted.size());
		compacted.push_back(buffer.vertices[index]);
	}
	for (std::uint32_t &index : buffer.indices) {
		index = remap[index];
	}
	buffer.vertices.swap(compacted);
}

} // namespace

WtVec3 wt_interpolated_mesh_normal(
	const WtCellSample &a,
	const WtCellSample &b,
	float isovalue
) noexcept {
	const float alpha = static_cast<float>(
		wt_regularized_isosurface_alpha(
			(static_cast<double>(isovalue) - static_cast<double>(a.density)) /
			(static_cast<double>(b.density) - static_cast<double>(a.density))
		)
	);
	WtVec3 normal = {
		a.gradient.x + (b.gradient.x - a.gradient.x) * alpha,
		a.gradient.y + (b.gradient.y - a.gradient.y) * alpha,
		a.gradient.z + (b.gradient.z - a.gradient.z) * alpha,
	};
	const float length = std::sqrt(
		normal.x * normal.x + normal.y * normal.y + normal.z * normal.z
	);
	if (length > 0.0F) {
		normal.x /= length;
		normal.y /= length;
		normal.z /= length;
	}
	return normal;
}

void wt_finalize_deformed_triangles(WtChunkMeshBuffer &buffer) {
	std::vector<TriangleRecord> triangles;
	triangles.reserve(buffer.indices.size() / 3U);
	for (std::size_t index = 0; index < buffer.indices.size(); index += 3) {
		const std::uint32_t index_a = buffer.indices[index];
		const std::uint32_t index_b = buffer.indices[index + 1];
		const std::uint32_t index_c = buffer.indices[index + 2];
		if (!triangle_is_valid(buffer, index_a, index_b, index_c)) {
			continue;
		}
		triangles.push_back({
			index_a,
			index_b,
			index_c,
			false,
			false,
		});
	}
	std::unordered_map<EdgeKey, std::vector<EdgeIncident>, EdgeKeyHash> edges;
	edges.reserve(triangles.size() * 3U);
	for (std::size_t triangle_index = 0; triangle_index < triangles.size();
		++triangle_index) {
		for (unsigned int edge_index = 0; edge_index < 3U; ++edge_index) {
			add_edge_incident(
				buffer, triangles, triangle_index, edge_index, edges
			);
		}
	}
	orient_consistent_components(buffer, triangles, edges);
	buffer.indices.clear();
	buffer.indices.reserve(triangles.size() * 3U);
	for (const TriangleRecord &triangle : triangles) {
		buffer.indices.push_back(triangle.a);
		if (triangle.flip) {
			buffer.indices.push_back(triangle.c);
			buffer.indices.push_back(triangle.b);
		} else {
			buffer.indices.push_back(triangle.b);
			buffer.indices.push_back(triangle.c);
		}
	}
	compact_unreferenced_vertices(buffer);
}

}
