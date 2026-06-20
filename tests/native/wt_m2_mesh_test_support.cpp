#include "wt_m2_mesh_test_support.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace world_transvoxel_test {
namespace {

QuantizedPoint quantize(
	const wt::WtVec3 &position,
	const wt::WtGridPoint &origin,
	const wt::WtGridPoint &reference = {}
) {
	constexpr double scale = 1000000.0;
	return {
		static_cast<std::int64_t>(std::llround(
			(static_cast<double>(origin.x - reference.x) + position.x) * scale)),
		static_cast<std::int64_t>(std::llround(
			(static_cast<double>(origin.y - reference.y) + position.y) * scale)),
		static_cast<std::int64_t>(std::llround(
			(static_cast<double>(origin.z - reference.z) + position.z) * scale)),
	};
}

Edge make_edge(QuantizedPoint a, QuantizedPoint b) {
	if (b < a) {
		const QuantizedPoint temporary = a;
		a = b;
		b = temporary;
	}
	return { a, b };
}

double axis_value(const wt::WtVec3 &position, const wt::WtGridPoint &origin, int axis) {
	if (axis == 0) return static_cast<double>(origin.x) + position.x;
	if (axis == 1) return static_cast<double>(origin.y) + position.y;
	return static_cast<double>(origin.z) + position.z;
}

void hash_byte(std::uint64_t &hash, std::uint8_t value) {
	hash ^= value;
	hash *= 1099511628211ULL;
}

void hash_u32(std::uint64_t &hash, std::uint32_t value) {
	for (unsigned int shift = 0; shift < 32; shift += 8) {
		hash_byte(hash, static_cast<std::uint8_t>(value >> shift));
	}
}

void hash_u64(std::uint64_t &hash, std::uint64_t value) {
	for (unsigned int shift = 0; shift < 64; shift += 8) {
		hash_byte(hash, static_cast<std::uint8_t>(value >> shift));
	}
}

void hash_float(std::uint64_t &hash, float value) {
	std::uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	hash_u32(hash, bits);
}

void hash_buffer(std::uint64_t &hash, const wt::WtChunkMeshBuffer &mesh) {
	hash_u32(hash, static_cast<std::uint32_t>(mesh.vertices.size()));
	hash_u32(hash, static_cast<std::uint32_t>(mesh.indices.size()));
	for (const wt::WtCellVertex &vertex : mesh.vertices) {
		hash_float(hash, vertex.position.x);
		hash_float(hash, vertex.position.y);
		hash_float(hash, vertex.position.z);
		hash_float(hash, vertex.normal.x);
		hash_float(hash, vertex.normal.y);
		hash_float(hash, vertex.normal.z);
		hash_u32(hash, vertex.material);
	}
	for (std::uint32_t index : mesh.indices) {
		hash_u32(hash, index);
	}
}

} // namespace

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

bool LinearSource::sample(
	const wt::WtGridPoint &point,
	wt::WtScalarSample &output
) const noexcept {
	const double value =
		0.37 * static_cast<double>(point.x) +
		0.53 * static_cast<double>(point.y) +
		0.71 * static_cast<double>(point.z) - threshold;
	output.density = static_cast<float>(value);
	output.material = value < 0.0 ? 7 : 3;
	return true;
}

bool SphereSource::sample(
	const wt::WtGridPoint &point,
	wt::WtScalarSample &output
) const noexcept {
	const double x = static_cast<double>(point.x - center.x);
	const double y = static_cast<double>(point.y - center.y);
	const double z = static_cast<double>(point.z - center.z);
	const double value = x * x + y * y + z * z - radius * radius;
	output.density = static_cast<float>(value);
	output.material = value < 0.0 ? 11 : 5;
	return true;
}

bool FailingSource::sample(const wt::WtGridPoint &, wt::WtScalarSample &) const noexcept {
	return false;
}

const wt::WtMeshingBackendInfo &InvalidBackend::get_info() const noexcept {
	static const wt::WtMeshingBackendInfo info = {
		"invalid_test", "0BSD", "test", 256, 512, false
	};
	return info;
}

bool InvalidBackend::is_available() const noexcept {
	return true;
}

wt::WtCellStatus InvalidBackend::mesh_regular_cell(
	const wt::WtRegularCellInput &,
	wt::WtCellMesh &output,
	wt::WtCellMeshingScratch &
) const noexcept {
	output.clear();
	output.vertex_count = 1;
	output.index_count = 3;
	output.indices[0] = 0;
	output.indices[1] = 0;
	output.indices[2] = 0;
	output.vertices[0].endpoint_a = 255;
	output.vertices[0].endpoint_b = 0;
	return wt::WtCellStatus::Ok;
}

wt::WtCellStatus InvalidBackend::mesh_transition_cell(
	const wt::WtTransitionCellInput &,
	wt::WtCellMesh &output,
	wt::WtCellMeshingScratch &
) const noexcept {
	output.clear();
	return wt::WtCellStatus::Empty;
}

bool QuantizedPoint::operator<(const QuantizedPoint &other) const noexcept {
	if (x != other.x) return x < other.x;
	if (y != other.y) return y < other.y;
	return z < other.z;
}

bool QuantizedPoint::operator==(const QuantizedPoint &other) const noexcept {
	return x == other.x && y == other.y && z == other.z;
}

bool Edge::operator<(const Edge &other) const noexcept {
	if (a < other.a) return true;
	if (other.a < a) return false;
	return b < other.b;
}

bool Edge::operator==(const Edge &other) const noexcept {
	return a == other.a && b == other.b;
}

EdgeSet plane_boundary_edges(
	const wt::WtChunkMeshBuffer &mesh,
	const wt::WtGridPoint &origin,
	int axis,
	double plane
) {
	EdgeCounts counts;
	for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
		const std::uint32_t triangle[3] = {
			mesh.indices[index], mesh.indices[index + 1], mesh.indices[index + 2]
		};
		for (unsigned int edge_index = 0; edge_index < 3; ++edge_index) {
			const wt::WtCellVertex &a = mesh.vertices[triangle[edge_index]];
			const wt::WtCellVertex &b = mesh.vertices[triangle[(edge_index + 1) % 3]];
			if (std::fabs(axis_value(a.position, origin, axis) - plane) < 0.00001 &&
				std::fabs(axis_value(b.position, origin, axis) - plane) < 0.00001) {
				++counts[make_edge(quantize(a.position, origin), quantize(b.position, origin))];
			}
		}
	}
	EdgeSet boundary;
	for (const auto &entry : counts) {
		if ((entry.second & 1U) != 0U) {
			boundary.insert(entry.first);
		}
	}
	return boundary;
}

void add_mesh_edges(
	const wt::WtChunkMeshBuffer &mesh,
	const wt::WtGridPoint &origin,
	EdgeCounts &counts,
	const wt::WtGridPoint &reference
) {
	for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
		const std::uint32_t triangle[3] = {
			mesh.indices[index], mesh.indices[index + 1], mesh.indices[index + 2]
		};
		for (unsigned int edge_index = 0; edge_index < 3; ++edge_index) {
			const wt::WtCellVertex &a = mesh.vertices[triangle[edge_index]];
			const wt::WtCellVertex &b = mesh.vertices[triangle[(edge_index + 1) % 3]];
			++counts[make_edge(
				quantize(a.position, origin, reference),
				quantize(b.position, origin, reference)
			)];
		}
	}
}

void add_result_edges(const wt::WtChunkMeshResult &result, EdgeCounts &counts) {
	add_mesh_edges(result.regular, result.world_origin, counts);
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		if ((result.transition_mask & wt::wt_face_bit(
			static_cast<wt::WtChunkFace>(face_index))) != 0) {
			add_mesh_edges(result.transitions[face_index], result.world_origin, counts);
		}
	}
}

void check_closed_surface(const EdgeCounts &counts, const char *message) {
	std::size_t invalid_count = 0;
	for (const auto &entry : counts) {
		if (entry.second != 2U) {
			++invalid_count;
		}
	}
	check(!counts.empty(), "closed seam gallery produced no surface");
	check(invalid_count == 0, message);
}

void validate_buffer(const wt::WtChunkMeshBuffer &mesh, const char *message) {
	check(mesh.vertices.size() <= mesh.vertex_limit, message);
	check(mesh.indices.size() <= mesh.index_limit, message);
	check((mesh.indices.size() % 3U) == 0U, message);
	std::vector<bool> referenced(mesh.vertices.size(), false);
	bool indices_valid = true;
	for (std::uint32_t index : mesh.indices) {
		const bool valid = index < mesh.vertices.size();
		check(valid, message);
		indices_valid &= valid;
		if (valid) referenced[index] = true;
	}
	if (indices_valid) {
		for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
			const wt::WtVec3 &a = mesh.vertices[mesh.indices[index]].position;
			const wt::WtVec3 &b = mesh.vertices[mesh.indices[index + 1]].position;
			const wt::WtVec3 &c = mesh.vertices[mesh.indices[index + 2]].position;
			const double ab_x = static_cast<double>(b.x) - a.x;
			const double ab_y = static_cast<double>(b.y) - a.y;
			const double ab_z = static_cast<double>(b.z) - a.z;
			const double ac_x = static_cast<double>(c.x) - a.x;
			const double ac_y = static_cast<double>(c.y) - a.y;
			const double ac_z = static_cast<double>(c.z) - a.z;
			const double cross_x = ab_y * ac_z - ab_z * ac_y;
			const double cross_y = ab_z * ac_x - ab_x * ac_z;
			const double cross_z = ab_x * ac_y - ab_y * ac_x;
			const double area_squared =
				cross_x * cross_x + cross_y * cross_y + cross_z * cross_z;
			check(std::isfinite(area_squared) && area_squared > 0.0,
				"chunk mesh retained a degenerate triangle");
		}
	}
	for (bool used : referenced) {
		check(used, "chunk mesh retained an unreferenced vertex");
	}
}

void hash_result(std::uint64_t &hash, const wt::WtChunkMeshResult &result) {
	hash_u32(hash, static_cast<std::uint32_t>(result.key.x));
	hash_u32(hash, static_cast<std::uint32_t>(result.key.y));
	hash_u32(hash, static_cast<std::uint32_t>(result.key.z));
	hash_byte(hash, result.key.lod);
	hash_u64(hash, static_cast<std::uint64_t>(result.world_origin.x));
	hash_u64(hash, static_cast<std::uint64_t>(result.world_origin.y));
	hash_u64(hash, static_cast<std::uint64_t>(result.world_origin.z));
	hash_byte(hash, result.transition_mask);
	hash_buffer(hash, result.regular);
	for (const wt::WtChunkMeshBuffer &transition : result.transitions) {
		hash_buffer(hash, transition);
	}
}

} // namespace world_transvoxel_test
