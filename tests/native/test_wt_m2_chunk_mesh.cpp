#include "backend/wt_transvoxel_mit_backend.h"
#include "meshing/wt_chunk_mesh_finalize.h"
#include "meshing/wt_chunk_mesher.h"
#include "wt_m2_mesh_test_support.h"

#include <cstdio>
#include <limits>
#include <map>
#include <vector>

namespace wt = world_transvoxel;
using namespace world_transvoxel_test;

namespace {

int face_axis(wt::WtChunkFace face) {
	return static_cast<int>(face) / 2;
}

bool positive_face(wt::WtChunkFace face) {
	return (static_cast<unsigned int>(face) & 1U) != 0U;
}

double triangle_alignment(const wt::WtChunkMeshBuffer &mesh, std::size_t index) {
	const std::uint32_t triangle[3] = {
		mesh.indices[index], mesh.indices[index + 1], mesh.indices[index + 2]
	};
	const wt::WtCellVertex &vertex_a = mesh.vertices[triangle[0]];
	const wt::WtCellVertex &vertex_b = mesh.vertices[triangle[1]];
	const wt::WtCellVertex &vertex_c = mesh.vertices[triangle[2]];
	const double ab_x =
		static_cast<double>(vertex_b.position.x) - vertex_a.position.x;
	const double ab_y =
		static_cast<double>(vertex_b.position.y) - vertex_a.position.y;
	const double ab_z =
		static_cast<double>(vertex_b.position.z) - vertex_a.position.z;
	const double ac_x =
		static_cast<double>(vertex_c.position.x) - vertex_a.position.x;
	const double ac_y =
		static_cast<double>(vertex_c.position.y) - vertex_a.position.y;
	const double ac_z =
		static_cast<double>(vertex_c.position.z) - vertex_a.position.z;
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
	return cross_x * normal_x + cross_y * normal_y + cross_z * normal_z;
}

struct FlatYSource final : wt::WtChunkSampleSource {
	double height = 7.0;

	bool sample(const wt::WtGridPoint &point, wt::WtScalarSample &output) const noexcept override {
		const double value = static_cast<double>(point.y) - height;
		output.density = static_cast<float>(value);
		output.material = value < 0.0 ? 7 : 3;
		return true;
	}
};

QuantizedPoint quantize_world(
	const wt::WtVec3 &position,
	const wt::WtGridPoint &origin
) {
	constexpr double scale = 1000000.0;
	return {
		static_cast<std::int64_t>(std::llround(
			(static_cast<double>(origin.x) + position.x) * scale)),
		static_cast<std::int64_t>(std::llround(
			(static_cast<double>(origin.y) + position.y) * scale)),
		static_cast<std::int64_t>(std::llround(
			(static_cast<double>(origin.z) + position.z) * scale)),
	};
}

Edge sorted_edge(QuantizedPoint a, QuantizedPoint b) {
	if (b < a) {
		const QuantizedPoint temporary = a;
		a = b;
		b = temporary;
	}
	return { a, b };
}

bool edge_forward(QuantizedPoint a, QuantizedPoint b) {
	return !(b < a);
}

double world_axis_value(
	const wt::WtVec3 &position,
	const wt::WtGridPoint &origin,
	int axis
) {
	if (axis == 0) return static_cast<double>(origin.x) + position.x;
	if (axis == 1) return static_cast<double>(origin.y) + position.y;
	return static_cast<double>(origin.z) + position.z;
}

void add_directed_plane_edges(
	const wt::WtChunkMeshBuffer &mesh,
	const wt::WtGridPoint &origin,
	int axis,
	double plane,
	std::map<Edge, std::pair<unsigned int, unsigned int>> &directions
) {
	for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
		const std::uint32_t triangle[3] = {
			mesh.indices[index], mesh.indices[index + 1], mesh.indices[index + 2]
		};
		for (unsigned int edge_index = 0; edge_index < 3; ++edge_index) {
			const wt::WtCellVertex &a = mesh.vertices[triangle[edge_index]];
			const wt::WtCellVertex &b = mesh.vertices[triangle[(edge_index + 1) % 3]];
			if (std::fabs(world_axis_value(a.position, origin, axis) - plane) >= 0.00001 ||
				std::fabs(world_axis_value(b.position, origin, axis) - plane) >= 0.00001) {
				continue;
			}
			const QuantizedPoint point_a = quantize_world(a.position, origin);
			const QuantizedPoint point_b = quantize_world(b.position, origin);
			std::pair<unsigned int, unsigned int> &counts =
				directions[sorted_edge(point_a, point_b)];
			if (edge_forward(point_a, point_b)) {
				++counts.first;
			} else {
				++counts.second;
			}
		}
	}
}

void add_directed_mesh_edges(
	const wt::WtChunkMeshBuffer &mesh,
	const wt::WtGridPoint &origin,
	std::map<Edge, std::pair<unsigned int, unsigned int>> &directions
) {
	for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
		const std::uint32_t triangle[3] = {
			mesh.indices[index], mesh.indices[index + 1], mesh.indices[index + 2]
		};
		for (unsigned int edge_index = 0; edge_index < 3; ++edge_index) {
			const wt::WtCellVertex &a = mesh.vertices[triangle[edge_index]];
			const wt::WtCellVertex &b = mesh.vertices[triangle[(edge_index + 1) % 3]];
			const QuantizedPoint point_a = quantize_world(a.position, origin);
			const QuantizedPoint point_b = quantize_world(b.position, origin);
			std::pair<unsigned int, unsigned int> &counts =
				directions[sorted_edge(point_a, point_b)];
			if (edge_forward(point_a, point_b)) {
				++counts.first;
			} else {
				++counts.second;
			}
		}
	}
}

void check_opposite_directed_shared_edges(
	const std::map<Edge, std::pair<unsigned int, unsigned int>> &directions,
	const char *message
) {
	std::size_t shared_count = 0;
	std::size_t conflicted_count = 0;
	for (const auto &entry : directions) {
		const unsigned int uses = entry.second.first + entry.second.second;
		if (uses != 2U) {
			continue;
		}
		++shared_count;
		if (entry.second.first != 1U || entry.second.second != 1U) {
			++conflicted_count;
		}
	}
	check(shared_count > 0, "same-LOD direction check found no shared seam edges");
	check(conflicted_count == 0, message);
}

std::vector<wt::WtChunkKey> fine_neighbors(wt::WtChunkFace face) {
	std::vector<wt::WtChunkKey> keys;
	for (int a = 0; a < 2; ++a) {
		for (int b = 0; b < 2; ++b) {
			switch (face) {
				case wt::WtChunkFace::NegativeX: keys.push_back({ -1, a, b, 0 }); break;
				case wt::WtChunkFace::PositiveX: keys.push_back({ 2, a, b, 0 }); break;
				case wt::WtChunkFace::NegativeY: keys.push_back({ a, -1, b, 0 }); break;
				case wt::WtChunkFace::PositiveY: keys.push_back({ a, 2, b, 0 }); break;
				case wt::WtChunkFace::NegativeZ: keys.push_back({ a, b, -1, 0 }); break;
				case wt::WtChunkFace::PositiveZ: keys.push_back({ a, b, 2, 0 }); break;
			}
		}
	}
	return keys;
}

double face_center_threshold(wt::WtChunkFace face) {
	const double coordinates[3] = {
		face_axis(face) == 0 ? (positive_face(face) ? 32.0 : 0.0) : 16.0,
		face_axis(face) == 1 ? (positive_face(face) ? 32.0 : 0.0) : 16.0,
		face_axis(face) == 2 ? (positive_face(face) ? 32.0 : 0.0) : 16.0,
	};
	return 0.37 * coordinates[0] + 0.53 * coordinates[1] +
		0.71 * coordinates[2] + 0.123;
}

void run_face_gallery(
	wt::WtChunkFace face,
	const wt::WtChunkMesher &mesher,
	wt::WtChunkMeshingScratch &scratch,
	std::uint64_t &hash
) {
	LinearSource source;
	source.threshold = face_center_threshold(face);
	const wt::WtChunkKey coarse_key = { 0, 0, 0, 1 };
	wt::WtChunkMeshResult coarse;
	const wt::WtChunkMeshingInput coarse_input = {
		coarse_key, wt::wt_face_bit(face), 0.0F, 0.25F
	};
	check(mesher.mesh(coarse_input, source, coarse, scratch) ==
		wt::WtChunkMeshingStatus::Ok, "coarse transition chunk failed");
	validate_buffer(coarse.regular, "invalid coarse regular buffer");
	const std::size_t face_index = static_cast<std::size_t>(face);
	validate_buffer(coarse.transitions[face_index], "invalid transition face buffer");
	check(!coarse.transitions[face_index].indices.empty(), "transition face is empty");

	const int axis = face_axis(face);
	const double full_plane = positive_face(face) ? 32.0 : 0.0;
	const double half_plane = positive_face(face) ? 31.5 : 0.5;
	const std::set<Edge> transition_full = plane_boundary_edges(
		coarse.transitions[face_index], coarse.world_origin, axis, full_plane
	);
	const std::set<Edge> transition_half = plane_boundary_edges(
		coarse.transitions[face_index], coarse.world_origin, axis, half_plane
	);
	const std::set<Edge> coarse_regular = plane_boundary_edges(
		coarse.regular, coarse.world_origin, axis, half_plane
	);
	check(!transition_full.empty(), "transition full-resolution contour is empty");
	check(transition_half == coarse_regular,
		"transition half-resolution contour does not match coarse regular mesh");

	std::set<Edge> fine_edges;
	for (const wt::WtChunkKey &fine_key : fine_neighbors(face)) {
		wt::WtChunkMeshResult fine;
		check(mesher.mesh({ fine_key, 0, 0.0F, 0.25F }, source, fine, scratch) ==
			wt::WtChunkMeshingStatus::Ok, "fine neighbor mesh failed");
		validate_buffer(fine.regular, "invalid fine regular buffer");
		const std::set<Edge> edges = plane_boundary_edges(
			fine.regular, fine.world_origin, axis, full_plane
		);
		fine_edges.insert(edges.begin(), edges.end());
		hash_result(hash, fine);
	}
	check(transition_full == fine_edges,
		"transition full-resolution contour does not match fine neighbors");
	hash_result(hash, coarse);
}

void test_same_lod_seam(
	const wt::WtChunkMesher &mesher,
	wt::WtChunkMeshingScratch &scratch,
	std::uint64_t &hash
) {
	LinearSource source;
	source.threshold = 0.53 * 8.0 + 0.71 * 8.0 + 0.123;
	wt::WtChunkMeshResult left;
	wt::WtChunkMeshResult right;
	check(mesher.mesh({ { 0, 0, 0, 0 }, 0, 0.0F, 0.25F }, source, left, scratch) ==
		wt::WtChunkMeshingStatus::Ok, "left same-LOD chunk failed");
	check(mesher.mesh({ { 1, 0, 0, 0 }, 0, 0.0F, 0.25F }, source, right, scratch) ==
		wt::WtChunkMeshingStatus::Ok, "right same-LOD chunk failed");
	const std::set<Edge> left_edges = plane_boundary_edges(left.regular, left.world_origin, 0, 16.0);
	const std::set<Edge> right_edges = plane_boundary_edges(right.regular, right.world_origin, 0, 16.0);
	check(!left_edges.empty() && left_edges == right_edges, "same-LOD seam mismatch");
	std::map<Edge, std::pair<unsigned int, unsigned int>> directions;
	add_directed_plane_edges(left.regular, left.world_origin, 0, 16.0, directions);
	add_directed_plane_edges(right.regular, right.world_origin, 0, 16.0, directions);
	check_opposite_directed_shared_edges(
		directions,
		"same-LOD seam retained an orientation-conflicted shared edge"
	);
	hash_result(hash, left);
	hash_result(hash, right);
}

void test_flat_lod0_seam_direction(
	const wt::WtChunkMesher &mesher,
	wt::WtChunkMeshingScratch &scratch,
	std::uint64_t &hash
) {
	FlatYSource source;
	source.height = 7.0;
	const wt::WtChunkKey south_key = { 69, 0, 58, 0 };
	const wt::WtChunkKey north_key = { 69, 0, 59, 0 };
	wt::WtChunkMeshResult south;
	wt::WtChunkMeshResult north;
	check(mesher.mesh({ south_key, 0, 0.0F, 0.25F }, source, south, scratch) ==
		wt::WtChunkMeshingStatus::Ok, "flat seam south chunk failed");
	check(mesher.mesh({ north_key, 0, 0.0F, 0.25F }, source, north, scratch) ==
		wt::WtChunkMeshingStatus::Ok, "flat seam north chunk failed");
	validate_buffer(south.regular, "invalid flat seam south buffer");
	validate_buffer(north.regular, "invalid flat seam north buffer");
	const double seam_z = static_cast<double>(wt::wt_chunk_bounds(north_key).minimum.z);
	const std::set<Edge> south_edges = plane_boundary_edges(
		south.regular, south.world_origin, 2, seam_z
	);
	const std::set<Edge> north_edges = plane_boundary_edges(
		north.regular, north.world_origin, 2, seam_z
	);
	check(!south_edges.empty() && south_edges == north_edges,
		"flat same-LOD seam edge positions mismatch");
	std::map<Edge, std::pair<unsigned int, unsigned int>> directions;
	add_directed_plane_edges(south.regular, south.world_origin, 2, seam_z, directions);
	add_directed_plane_edges(north.regular, north.world_origin, 2, seam_z, directions);
	check_opposite_directed_shared_edges(
		directions,
		"flat same-LOD seam retained an orientation-conflicted shared edge"
	);
	hash_result(hash, south);
	hash_result(hash, north);
}

void test_finalizer_orients_inverted_connected_component() {
	wt::WtChunkMeshBuffer mesh;
	mesh.prepare(4, 6);
	const wt::WtVec3 up = { 0.0F, 0.0F, 1.0F };
	mesh.vertices.push_back({ { 0.0F, 0.0F, 0.0F }, up, 1, 0, 0 });
	mesh.vertices.push_back({ { 1.0F, 0.0F, 0.0F }, up, 1, 0, 0 });
	mesh.vertices.push_back({ { 1.0F, 1.0F, 0.0F }, up, 1, 0, 0 });
	mesh.vertices.push_back({ { 0.0F, 1.0F, 0.0F }, up, 1, 0, 0 });
	mesh.indices = { 0, 2, 1, 0, 3, 2 };
	wt::wt_finalize_deformed_triangles(mesh);
	validate_buffer(mesh, "invalid finalizer component-anchor mesh");
	check(mesh.indices.size() == 6U,
		"finalizer component-anchor mesh lost triangles");
	check(triangle_alignment(mesh, 0) > 0.0 &&
		triangle_alignment(mesh, 3) > 0.0,
		"finalizer kept an inverted connected component");
}

void test_finalizer_rejects_near_zero_area_slivers() {
	wt::WtChunkMeshBuffer mesh;
	mesh.prepare(3, 3);
	const wt::WtVec3 up = { 0.0F, 0.0F, 1.0F };
	mesh.vertices.push_back({ { 0.0F, 0.0F, 0.0F }, up, 1, 0, 0 });
	mesh.vertices.push_back({ { 1.0F, 0.0F, 0.0F }, up, 1, 0, 0 });
	mesh.vertices.push_back({ { 1.0F, 0.0000001F, 0.0F }, up, 1, 0, 0 });
	mesh.indices = { 0, 1, 2 };
	wt::wt_finalize_deformed_triangles(mesh);
	check(mesh.indices.empty(),
		"finalizer retained a near-zero-area deformed sliver triangle");
}

void test_finalizer_orients_quantized_edge_component() {
	wt::WtChunkMeshBuffer mesh;
	mesh.prepare(6, 6);
	const wt::WtVec3 up = { 0.0F, 0.0F, 1.0F };
	mesh.vertices.push_back({ { 0.0F, 0.0F, 0.0F }, up, 1, 0, 0 });
	mesh.vertices.push_back({ { 1.0F, 0.0F, 0.0F }, up, 1, 0, 0 });
	mesh.vertices.push_back({ { 0.0F, 1.0F, 0.0F }, up, 1, 0, 0 });
	mesh.vertices.push_back({ { 1.0001F, 0.0F, 0.0F }, up, 1, 0, 0 });
	mesh.vertices.push_back({ { 0.0F, 1.0001F, 0.0F }, up, 1, 0, 0 });
	mesh.vertices.push_back({ { 1.0F, 1.0F, 0.0F }, up, 1, 0, 0 });
	mesh.indices = { 0, 1, 2, 3, 4, 5 };
	wt::wt_finalize_deformed_triangles(mesh);
	validate_buffer(mesh, "invalid finalizer quantized-edge mesh");
	check(mesh.indices.size() == 6U,
		"finalizer quantized-edge mesh lost triangles");
	check(triangle_alignment(mesh, 0) > 0.0 &&
		triangle_alignment(mesh, 3) > 0.0,
		"finalizer did not propagate orientation across quantized edge");
}

void test_extreme_same_lod_gallery(
	const wt::WtChunkMesher &mesher,
	wt::WtChunkMeshingScratch &scratch,
	std::uint64_t &hash,
	const wt::WtChunkKey &left_key
) {
	wt::WtChunkKey right_key = left_key;
	++right_key.x;
	const wt::WtChunkBounds left_bounds = wt::wt_chunk_bounds(left_key);
	const std::int64_t spacing = wt::wt_lod_cell_size(left_key.lod);
	SphereSource source;
	source.center = {
		left_bounds.maximum.x,
		(left_bounds.minimum.y + left_bounds.maximum.y) / 2,
		(left_bounds.minimum.z + left_bounds.maximum.z) / 2,
	};
	source.radius = 6.25 * static_cast<double>(spacing);
	wt::WtChunkMeshResult left;
	wt::WtChunkMeshResult right;
	check(mesher.mesh({ left_key, 0, 0.0F, 0.25F }, source, left, scratch) ==
		wt::WtChunkMeshingStatus::Ok, "extreme-coordinate left chunk failed");
	check(mesher.mesh({ right_key, 0, 0.0F, 0.25F }, source, right, scratch) ==
		wt::WtChunkMeshingStatus::Ok, "extreme-coordinate right chunk failed");
	std::map<Edge, unsigned int> edge_counts;
	add_mesh_edges(left.regular, left.world_origin, edge_counts, left_bounds.minimum);
	add_mesh_edges(right.regular, right.world_origin, edge_counts, left_bounds.minimum);
	check_closed_surface(edge_counts, "extreme-coordinate same-LOD gallery is open");
	std::map<Edge, std::pair<unsigned int, unsigned int>> directions;
	add_directed_mesh_edges(left.regular, left.world_origin, directions);
	add_directed_mesh_edges(right.regular, right.world_origin, directions);
	check_opposite_directed_shared_edges(
		directions,
		"extreme-coordinate same-LOD gallery retained orientation-conflicted shared edges"
	);
	hash_result(hash, left);
	hash_result(hash, right);
}

void test_multi_face_gallery(
	const wt::WtChunkMesher &mesher,
	wt::WtChunkMeshingScratch &scratch,
	std::uint64_t &hash,
	const wt::WtChunkKey &coarse_key,
	const std::array<int, 3> &face_signs
) {
	const wt::WtChunkBounds bounds = wt::wt_chunk_bounds(coarse_key);
	const std::int32_t coarse_coordinates[3] = {
		coarse_key.x, coarse_key.y, coarse_key.z
	};
	const std::int64_t minimum[3] = {
		bounds.minimum.x, bounds.minimum.y, bounds.minimum.z
	};
	const std::int64_t maximum[3] = {
		bounds.maximum.x, bounds.maximum.y, bounds.maximum.z
	};
	std::int64_t center_coordinates[3]{};
	std::int32_t fine_options[3][2]{};
	unsigned int active_axes = 0;
	std::uint8_t mask = 0;
	for (unsigned int axis = 0; axis < 3; ++axis) {
		const std::int32_t first_child = coarse_coordinates[axis] * 2;
		if (face_signs[axis] < 0) {
			center_coordinates[axis] = minimum[axis];
			fine_options[axis][0] = first_child;
			fine_options[axis][1] = first_child - 1;
			mask |= wt::wt_face_bit(static_cast<wt::WtChunkFace>(axis * 2));
			++active_axes;
		} else if (face_signs[axis] > 0) {
			center_coordinates[axis] = maximum[axis];
			fine_options[axis][0] = first_child + 1;
			fine_options[axis][1] = first_child + 2;
			mask |= wt::wt_face_bit(static_cast<wt::WtChunkFace>(axis * 2 + 1));
			++active_axes;
		} else {
			center_coordinates[axis] = (minimum[axis] + maximum[axis]) / 2;
			fine_options[axis][0] = first_child;
			fine_options[axis][1] = first_child + 1;
		}
	}
	SphereSource source;
	source.center = {
		center_coordinates[0], center_coordinates[1], center_coordinates[2]
	};
	wt::WtChunkMeshResult coarse;
	check(mesher.mesh({ coarse_key, mask, 0.0F, 0.25F }, source, coarse, scratch) ==
		wt::WtChunkMeshingStatus::Ok, "multi-face coarse chunk failed");
	validate_buffer(coarse.regular, "invalid multi-face regular buffer");
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		const bool active = (mask & wt::wt_face_bit(
			static_cast<wt::WtChunkFace>(face_index))) != 0;
		check(active ? !coarse.transitions[face_index].indices.empty() :
			coarse.transitions[face_index].indices.empty(),
			"multi-face transition buffer activation mismatch");
		validate_buffer(coarse.transitions[face_index], "invalid multi-face transition buffer");
	}
	std::map<Edge, unsigned int> edge_counts;
	add_result_edges(coarse, edge_counts);
	hash_result(hash, coarse);

	for (unsigned int selection = 0; selection < 8; ++selection) {
		bool outside_active_face = false;
		for (unsigned int axis = 0; axis < 3; ++axis) {
			outside_active_face |= face_signs[axis] != 0 &&
				((selection & (1U << axis)) != 0);
		}
		if (!outside_active_face) {
			continue;
		}
		const wt::WtChunkKey fine_key = {
			fine_options[0][(selection & 1U) != 0],
			fine_options[1][(selection & 2U) != 0],
			fine_options[2][(selection & 4U) != 0],
			0,
		};
		wt::WtChunkMeshResult fine;
		check(mesher.mesh({ fine_key, 0, 0.0F, 0.25F }, source, fine, scratch) ==
			wt::WtChunkMeshingStatus::Ok, "multi-face fine chunk failed");
		validate_buffer(fine.regular, "invalid multi-face fine buffer");
		add_result_edges(fine, edge_counts);
		hash_result(hash, fine);
	}
	check_closed_surface(
		edge_counts,
		active_axes == 2 ? "two-face LOD edge gallery is open" :
			"three-face LOD corner gallery is open"
	);
}

void test_convex_refined_corner_gallery(
	const wt::WtChunkMesher &mesher,
	wt::WtChunkMeshingScratch &scratch,
	std::uint64_t &hash
) {
	SphereSource source;
	source.center = { 32, 16, 32 };
	source.radius = 12.25;
	std::map<Edge, unsigned int> edge_counts;
	const std::array<std::pair<wt::WtChunkKey, std::uint8_t>, 3> coarse = {{
		{ { 0, 0, 0, 1 }, 0 },
		{ { 1, 0, 0, 1 }, wt::wt_face_bit(wt::WtChunkFace::PositiveZ) },
		{ { 0, 0, 1, 1 }, wt::wt_face_bit(wt::WtChunkFace::PositiveX) },
	}};
	for (const auto &[key, mask] : coarse) {
		wt::WtChunkMeshResult result;
		check(mesher.mesh({ key, mask, 0.0F, 0.25F }, source, result, scratch) ==
			wt::WtChunkMeshingStatus::Ok,
			"convex-corner coarse chunk failed");
		validate_buffer(result.regular, "invalid convex-corner coarse regular mesh");
		add_result_edges(result, edge_counts);
		hash_result(hash, result);
	}
	for (std::int32_t z = 2; z < 4; ++z) {
		for (std::int32_t y = 0; y < 2; ++y) {
			for (std::int32_t x = 2; x < 4; ++x) {
				wt::WtChunkMeshResult result;
				check(mesher.mesh(
					{ { x, y, z, 0 }, 0, 0.0F, 0.25F },
					source,
					result,
					scratch
				) == wt::WtChunkMeshingStatus::Ok,
					"convex-corner fine chunk failed");
				validate_buffer(result.regular,
					"invalid convex-corner fine regular mesh");
				add_result_edges(result, edge_counts);
				hash_result(hash, result);
			}
		}
	}
	check_closed_surface(
		edge_counts,
		"convex refined-region LOD corner is open"
	);
}

void test_errors(const wt::WtChunkMesher &mesher, wt::WtChunkMeshingScratch &scratch) {
	LinearSource source;
	wt::WtChunkMeshResult output;
	check(mesher.mesh({ { 0, 0, 0, 0 }, 1, 0.0F, 0.25F }, source, output, scratch) ==
		wt::WtChunkMeshingStatus::InvalidInput, "LOD0 transition mask accepted");
	check(mesher.mesh({ { 0, 0, 0, 1 }, 0, 0.0F,
		std::numeric_limits<float>::quiet_NaN() }, source, output, scratch) ==
		wt::WtChunkMeshingStatus::InvalidInput, "NaN transition width accepted");
	FailingSource failing;
	check(mesher.mesh({ { 0, 0, 0, 0 }, 0, 0.0F, 0.25F }, failing, output, scratch) ==
		wt::WtChunkMeshingStatus::SampleSourceFailure, "sample failure was not reported");
	check(output.regular.indices.empty(), "failed mesh retained output");
	InvalidBackend invalid_backend;
	const wt::WtChunkMesher invalid_mesher(invalid_backend);
	check(invalid_mesher.mesh({ { 0, 0, 0, 0 }, 0, 0.0F, 0.25F },
		source, output, scratch) == wt::WtChunkMeshingStatus::CellBackendFailure,
		"invalid backend provenance was accepted");
	check(output.regular.indices.empty(), "backend failure retained output");
}

} // namespace

int main() {
	const wt::WtChunkMesher mesher(wt::wt_get_transvoxel_mit_backend());
	wt::WtChunkMeshingScratch scratch;
	std::uint64_t hash = 14695981039346656037ULL;
	test_finalizer_orients_inverted_connected_component();
	test_finalizer_rejects_near_zero_area_slivers();
	test_finalizer_orients_quantized_edge_component();
	test_same_lod_seam(mesher, scratch, hash);
	test_flat_lod0_seam_direction(mesher, scratch, hash);
	test_extreme_same_lod_gallery(
		mesher,
		scratch,
		hash,
		{ std::numeric_limits<std::int32_t>::max() - 1, 0, 0, wt::kWtMaximumLod }
	);
	test_extreme_same_lod_gallery(
		mesher,
		scratch,
		hash,
		{ std::numeric_limits<std::int32_t>::min(), 0, 0, wt::kWtMaximumLod }
	);
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		run_face_gallery(static_cast<wt::WtChunkFace>(face_index), mesher, scratch, hash);
	}
	for (unsigned int inactive_axis = 0; inactive_axis < 3; ++inactive_axis) {
		for (int first_sign : { -1, 1 }) {
			for (int second_sign : { -1, 1 }) {
				std::array<int, 3> signs = { first_sign, second_sign, second_sign };
				signs[inactive_axis] = 0;
				const unsigned int first_active = (inactive_axis + 1) % 3;
				const unsigned int second_active = (inactive_axis + 2) % 3;
				signs[first_active] = first_sign;
				signs[second_active] = second_sign;
				test_multi_face_gallery(
					mesher, scratch, hash, { 0, 0, 0, 1 }, signs
				);
			}
		}
	}
	for (int x_sign : { -1, 1 }) {
		for (int y_sign : { -1, 1 }) {
			for (int z_sign : { -1, 1 }) {
				test_multi_face_gallery(
					mesher,
					scratch,
					hash,
					{ 0, 0, 0, 1 },
					{ x_sign, y_sign, z_sign }
				);
			}
		}
	}
	test_multi_face_gallery(
		mesher, scratch, hash, { -1, -1, -1, 1 }, { 1, 1, 1 }
	);
	test_convex_refined_corner_gallery(mesher, scratch, hash);
	test_errors(mesher, scratch);

	constexpr std::uint64_t expected_hash = 0xaa3479d9633a83aaULL;
	check(hash == expected_hash, "M2 chunk aggregate hash mismatch");
	std::printf("M2_MESH_HASH %016llx\n", static_cast<unsigned long long>(hash));
	if (failure_count != 0) {
		std::fprintf(stderr, "M2_CHUNK_MESH_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M2_CHUNK_MESH_PASS same_lod=3 transition_faces=6 edge_galleries=12 "
		"corner_galleries=9 convex_refined_corners=1 winding=outward\n");
	return 0;
}
