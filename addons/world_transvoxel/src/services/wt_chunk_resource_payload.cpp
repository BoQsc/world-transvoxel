#include "services/wt_chunk_resource_payload.h"

#include <cmath>

namespace world_transvoxel {
namespace {

bool finite_vec3(const WtVec3 &value) noexcept {
	return std::isfinite(value.x) &&
		std::isfinite(value.y) &&
		std::isfinite(value.z);
}

bool equal_vec3(const WtVec3 &left, const WtVec3 &right) noexcept {
	return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool equal_vertex(
	const WtCellVertex &left,
	const WtCellVertex &right
) noexcept {
	return equal_vec3(left.position, right.position) &&
		equal_vec3(left.normal, right.normal) &&
		left.material == right.material &&
		left.endpoint_a == right.endpoint_a &&
		left.endpoint_b == right.endpoint_b;
}

bool valid_mesh_buffer(
	const WtChunkMeshBuffer &buffer,
	std::size_t maximum_vertices,
	std::size_t maximum_indices
) noexcept {
	if (buffer.vertices.size() > maximum_vertices ||
		buffer.indices.size() > maximum_indices ||
		(buffer.indices.size() % 3U) != 0) {
		return false;
	}
	for (const WtCellVertex &vertex : buffer.vertices) {
		if (!finite_vec3(vertex.position) || !finite_vec3(vertex.normal) ||
			vertex.endpoint_a >= kWtTransitionTopologySampleCount ||
			vertex.endpoint_b >= kWtTransitionTopologySampleCount) {
			return false;
		}
	}
	for (std::uint32_t index : buffer.indices) {
		if (index >= buffer.vertices.size()) {
			return false;
		}
	}
	return true;
}

bool equal_mesh_buffer(
	const WtChunkMeshBuffer &left,
	const WtChunkMeshBuffer &right
) noexcept {
	if (left.vertices.size() != right.vertices.size() ||
		left.indices != right.indices) {
		return false;
	}
	for (std::size_t index = 0; index < left.vertices.size(); ++index) {
		if (!equal_vertex(left.vertices[index], right.vertices[index])) {
			return false;
		}
	}
	return true;
}

} // namespace

bool wt_is_valid_mesh_payload(const WtChunkMeshResult &mesh) noexcept {
	if (!wt_is_valid_chunk_key(mesh.key) ||
		mesh.world_origin != wt_chunk_bounds(mesh.key).minimum ||
		(mesh.transition_mask & 0xc0U) != 0 ||
		(mesh.key.lod == 0 && mesh.transition_mask != 0) ||
		!valid_mesh_buffer(
			mesh.regular,
			kWtMaximumRegularChunkVertices,
			kWtMaximumRegularChunkIndices
		)) {
		return false;
	}
	for (std::size_t face = 0; face < mesh.transitions.size(); ++face) {
		const bool active = (mesh.transition_mask & wt_face_bit(
			static_cast<WtChunkFace>(face))) != 0;
		const WtChunkMeshBuffer &buffer = mesh.transitions[face];
		if ((!active &&
				(!buffer.vertices.empty() || !buffer.indices.empty())) ||
			!valid_mesh_buffer(
				buffer,
				kWtMaximumTransitionFaceVertices,
				kWtMaximumTransitionFaceIndices
			)) {
			return false;
		}
	}
	return true;
}

bool wt_equal_mesh_payload(
	const WtChunkMeshResult &left,
	const WtChunkMeshResult &right
) noexcept {
	if (left.key != right.key ||
		left.world_origin != right.world_origin ||
		left.transition_mask != right.transition_mask ||
		!equal_mesh_buffer(left.regular, right.regular)) {
		return false;
	}
	for (std::size_t face = 0; face < left.transitions.size(); ++face) {
		if (!equal_mesh_buffer(left.transitions[face], right.transitions[face])) {
			return false;
		}
	}
	return true;
}

std::size_t wt_mesh_payload_resident_bytes(
	const WtChunkMeshResult &mesh
) noexcept {
	std::size_t size = sizeof(WtChunkMeshResult) +
		mesh.regular.vertices.capacity() * sizeof(WtCellVertex) +
		mesh.regular.indices.capacity() * sizeof(std::uint32_t);
	for (const WtChunkMeshBuffer &buffer : mesh.transitions) {
		size += buffer.vertices.capacity() * sizeof(WtCellVertex) +
			buffer.indices.capacity() * sizeof(std::uint32_t);
	}
	return size;
}

bool wt_is_valid_render_payload(const WtRenderPayload &render) noexcept {
	if (!wt_is_valid_chunk_key(render.key) ||
		render.generation.value == 0 ||
		render.world_origin != wt_chunk_bounds(render.key).minimum ||
		render.vertices.size() > kWtMaximumRenderVertices ||
		render.indices.size() > kWtMaximumRenderIndices ||
		(render.indices.size() % 3U) != 0) {
		return false;
	}
	for (const WtRenderVertex &vertex : render.vertices) {
		if (!finite_vec3(vertex.position) || !finite_vec3(vertex.normal)) {
			return false;
		}
	}
	for (std::uint32_t index : render.indices) {
		if (index >= render.vertices.size()) {
			return false;
		}
	}
	return true;
}

bool wt_equal_render_payload(
	const WtRenderPayload &left,
	const WtRenderPayload &right
) noexcept {
	if (left.key != right.key ||
		left.generation != right.generation ||
		left.world_origin != right.world_origin ||
		left.vertices.size() != right.vertices.size() ||
		left.indices != right.indices) {
		return false;
	}
	for (std::size_t index = 0; index < left.vertices.size(); ++index) {
		const WtRenderVertex &a = left.vertices[index];
		const WtRenderVertex &b = right.vertices[index];
		if (!equal_vec3(a.position, b.position) ||
			!equal_vec3(a.normal, b.normal) ||
			a.material != b.material) {
			return false;
		}
	}
	return true;
}

std::size_t wt_render_payload_resident_bytes(
	const WtRenderPayload &render
) noexcept {
	return sizeof(WtRenderPayload) +
		render.vertices.capacity() * sizeof(WtRenderVertex) +
		render.indices.capacity() * sizeof(std::uint32_t);
}

bool wt_is_valid_collision_payload(
	const WtCollisionPayload &collision
) noexcept {
	const std::size_t maximum_triangles = kWtMaximumRenderIndices / 3;
	if (!wt_is_valid_chunk_key(collision.key) ||
		collision.generation.value == 0 ||
		collision.world_origin != wt_chunk_bounds(collision.key).minimum ||
		collision.faces.size() > kWtMaximumRenderIndices ||
		(collision.faces.size() % 3U) != 0 ||
		collision.metrics.input_triangles > maximum_triangles ||
		collision.metrics.output_triangles > maximum_triangles ||
		collision.metrics.degenerate_triangles > maximum_triangles ||
		collision.metrics.thin_triangles > maximum_triangles ||
		collision.metrics.output_triangles * 3 != collision.faces.size() ||
		collision.metrics.input_triangles !=
			collision.metrics.output_triangles +
			collision.metrics.degenerate_triangles +
			collision.metrics.thin_triangles) {
		return false;
	}
	for (const WtVec3 &face : collision.faces) {
		if (!finite_vec3(face)) {
			return false;
		}
	}
	return true;
}

bool wt_equal_collision_payload(
	const WtCollisionPayload &left,
	const WtCollisionPayload &right
) noexcept {
	if (left.key != right.key ||
		left.generation != right.generation ||
		left.world_origin != right.world_origin ||
		left.faces.size() != right.faces.size() ||
		left.metrics.input_triangles != right.metrics.input_triangles ||
		left.metrics.output_triangles != right.metrics.output_triangles ||
		left.metrics.degenerate_triangles != right.metrics.degenerate_triangles ||
		left.metrics.thin_triangles != right.metrics.thin_triangles) {
		return false;
	}
	for (std::size_t index = 0; index < left.faces.size(); ++index) {
		if (!equal_vec3(left.faces[index], right.faces[index])) {
			return false;
		}
	}
	return true;
}

std::size_t wt_collision_payload_resident_bytes(
	const WtCollisionPayload &collision
) noexcept {
	return sizeof(WtCollisionPayload) +
		collision.faces.capacity() * sizeof(WtVec3);
}

} // namespace world_transvoxel
