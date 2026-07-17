#include "meshing/wt_transition_face_constraints.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace world_transvoxel {
namespace {

using WideSigned = __int128_t;

struct QuantizedPoint {
	std::int64_t x = 0;
	std::int64_t y = 0;
	std::int64_t z = 0;

	bool operator==(const QuantizedPoint &other) const noexcept {
		return x == other.x && y == other.y && z == other.z;
	}

	bool operator<(const QuantizedPoint &other) const noexcept {
		if (x != other.x) return x < other.x;
		if (y != other.y) return y < other.y;
		return z < other.z;
	}
};

struct ConstraintVertex {
	QuantizedPoint point;
	std::uint32_t index = 0;
	WideSigned distance = 0;
};

QuantizedPoint quantize(const WtVec3 &point) noexcept {
	constexpr double scale = 65536.0;
	return {
		static_cast<std::int64_t>(std::llround(
			static_cast<double>(point.x) * scale
		)),
		static_cast<std::int64_t>(std::llround(
			static_cast<double>(point.y) * scale
		)),
		static_cast<std::int64_t>(std::llround(
			static_cast<double>(point.z) * scale
		)),
	};
}

std::int64_t face_coordinate(
	const QuantizedPoint &point,
	WtChunkFace face
) noexcept {
	switch (face) {
		case WtChunkFace::NegativeX:
		case WtChunkFace::PositiveX:
			return point.x;
		case WtChunkFace::NegativeY:
		case WtChunkFace::PositiveY:
			return point.y;
		case WtChunkFace::NegativeZ:
		case WtChunkFace::PositiveZ:
			return point.z;
	}
	return 0;
}

bool positive_face(WtChunkFace face) noexcept {
	return (static_cast<unsigned int>(face) & 1U) != 0;
}

WideSigned dot(
	const QuantizedPoint &a,
	const QuantizedPoint &b
) noexcept {
	return static_cast<WideSigned>(a.x) * b.x +
		static_cast<WideSigned>(a.y) * b.y +
		static_cast<WideSigned>(a.z) * b.z;
}

WideSigned magnitude(WideSigned value) noexcept {
	return value < 0 ? -value : value;
}

QuantizedPoint subtract(
	const QuantizedPoint &a,
	const QuantizedPoint &b
) noexcept {
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

bool collinear_between(
	const QuantizedPoint &point,
	const QuantizedPoint &a,
	const QuantizedPoint &b,
	WideSigned &distance
) noexcept {
	if (point == a || point == b) {
		return false;
	}
	const QuantizedPoint direction = subtract(b, a);
	const QuantizedPoint relative = subtract(point, a);
	const WideSigned cross_x =
		static_cast<WideSigned>(relative.y) * direction.z -
		static_cast<WideSigned>(relative.z) * direction.y;
	const WideSigned cross_y =
		static_cast<WideSigned>(relative.z) * direction.x -
		static_cast<WideSigned>(relative.x) * direction.z;
	const WideSigned cross_z =
		static_cast<WideSigned>(relative.x) * direction.y -
		static_cast<WideSigned>(relative.y) * direction.x;
	const WideSigned tolerance = std::max({
		magnitude(static_cast<WideSigned>(direction.x)),
		magnitude(static_cast<WideSigned>(direction.y)),
		magnitude(static_cast<WideSigned>(direction.z)),
	});
	if (magnitude(cross_x) > tolerance ||
		magnitude(cross_y) > tolerance ||
		magnitude(cross_z) > tolerance) {
		return false;
	}
	distance = dot(relative, direction);
	const WideSigned length_squared = dot(direction, direction);
	return distance > 0 && distance < length_squared;
}

std::vector<ConstraintVertex> edge_constraints(
	std::uint32_t index_a,
	std::uint32_t index_b,
	const std::vector<QuantizedPoint> &points,
	const std::vector<std::uint32_t> &face_vertices
) {
	std::vector<ConstraintVertex> constraints;
	const QuantizedPoint &a = points[index_a];
	const QuantizedPoint &b = points[index_b];
	for (std::uint32_t candidate : face_vertices) {
		WideSigned distance = 0;
		if (collinear_between(points[candidate], a, b, distance)) {
			constraints.push_back({ points[candidate], candidate, distance });
		}
	}
	std::sort(
		constraints.begin(),
		constraints.end(),
		[](const ConstraintVertex &left, const ConstraintVertex &right) {
			if (left.distance != right.distance) {
				return left.distance < right.distance;
			}
			return left.index < right.index;
		}
	);
	constraints.erase(
		std::unique(
			constraints.begin(),
			constraints.end(),
			[](const ConstraintVertex &left, const ConstraintVertex &right) {
				return left.point == right.point;
			}
		),
		constraints.end()
	);
	return constraints;
}

} // namespace

bool wt_preserve_transition_face_constraints(
	WtChunkMeshBuffer &buffer,
	WtChunkFace face,
	float extent
) {
	if (!std::isfinite(extent) || extent <= 0.0F ||
		(buffer.indices.size() % 3U) != 0) {
		return false;
	}
	std::vector<QuantizedPoint> points;
	points.reserve(buffer.vertices.size());
	for (const WtCellVertex &vertex : buffer.vertices) {
		points.push_back(quantize(vertex.position));
	}
	const std::int64_t plane = positive_face(face) ?
		quantize({ extent, extent, extent }).x : 0;
	std::map<QuantizedPoint, std::uint32_t> unique_face_vertices;
	for (std::uint32_t index = 0;
		index < points.size();
		++index) {
		if (face_coordinate(points[index], face) == plane) {
			unique_face_vertices.emplace(points[index], index);
		}
	}
	std::vector<std::uint32_t> face_vertices;
	face_vertices.reserve(unique_face_vertices.size());
	for (const auto &entry : unique_face_vertices) {
		face_vertices.push_back(entry.second);
	}

	using Triangle = std::array<std::uint32_t, 3>;
	std::vector<Triangle> pending;
	pending.reserve(buffer.indices.size() / 3U);
	for (std::size_t index = 0; index < buffer.indices.size(); index += 3) {
		if (buffer.indices[index] >= points.size() ||
			buffer.indices[index + 1] >= points.size() ||
			buffer.indices[index + 2] >= points.size()) {
			return false;
		}
		pending.push_back({
			buffer.indices[index],
			buffer.indices[index + 1],
			buffer.indices[index + 2],
		});
	}
	std::vector<Triangle> constrained;
	constrained.reserve(pending.size());
	const std::size_t triangle_limit = buffer.index_limit / 3U;
	while (!pending.empty()) {
		const Triangle triangle = pending.back();
		pending.pop_back();
		bool split = false;
		for (unsigned int edge = 0; edge < 3 && !split; ++edge) {
			const std::uint32_t index_a = triangle[edge];
			const std::uint32_t index_b = triangle[(edge + 1U) % 3U];
			const std::uint32_t index_c = triangle[(edge + 2U) % 3U];
			if (face_coordinate(points[index_a], face) != plane ||
				face_coordinate(points[index_b], face) != plane) {
				continue;
			}
			const std::vector<ConstraintVertex> constraints =
				edge_constraints(
					index_a, index_b, points, face_vertices
				);
			if (constraints.empty()) {
				continue;
			}
			const std::size_t added = constraints.size() + 1U;
			if (constrained.size() + pending.size() > triangle_limit ||
				added > triangle_limit -
					(constrained.size() + pending.size())) {
				return false;
			}
			std::uint32_t segment_a = index_a;
			for (const ConstraintVertex &constraint : constraints) {
				pending.push_back({ segment_a, constraint.index, index_c });
				segment_a = constraint.index;
			}
			pending.push_back({ segment_a, index_b, index_c });
			split = true;
		}
		if (!split) {
			if (constrained.size() >= triangle_limit) {
				return false;
			}
			constrained.push_back(triangle);
		}
	}
	buffer.indices.clear();
	buffer.indices.reserve(constrained.size() * 3U);
	for (const Triangle &triangle : constrained) {
		buffer.indices.insert(
			buffer.indices.end(), triangle.begin(), triangle.end()
		);
	}
	return true;
}

} // namespace world_transvoxel
