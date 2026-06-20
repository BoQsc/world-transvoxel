#include "render/wt_render_payload.h"

#include <cmath>

namespace world_transvoxel {
namespace {

bool is_finite(const WtVec3 &value) noexcept {
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

double triangle_area_squared(
	const WtVec3 &a,
	const WtVec3 &b,
	const WtVec3 &c
) noexcept {
	const double ab_x = static_cast<double>(b.x) - a.x;
	const double ab_y = static_cast<double>(b.y) - a.y;
	const double ab_z = static_cast<double>(b.z) - a.z;
	const double ac_x = static_cast<double>(c.x) - a.x;
	const double ac_y = static_cast<double>(c.y) - a.y;
	const double ac_z = static_cast<double>(c.z) - a.z;
	const double cross_x = ab_y * ac_z - ab_z * ac_y;
	const double cross_y = ab_z * ac_x - ab_x * ac_z;
	const double cross_z = ab_x * ac_y - ab_y * ac_x;
	return cross_x * cross_x + cross_y * cross_y + cross_z * cross_z;
}

WtRenderBuildStatus append_buffer(
	const WtChunkMeshBuffer &source,
	WtRenderPayload &output
) {
	if ((source.indices.size() % 3U) != 0) {
		return WtRenderBuildStatus::InvalidMesh;
	}
	if (source.vertices.size() > kWtMaximumRenderVertices - output.vertices.size() ||
		source.indices.size() > kWtMaximumRenderIndices - output.indices.size()) {
		return WtRenderBuildStatus::CapacityExceeded;
	}
	const std::uint32_t base = static_cast<std::uint32_t>(output.vertices.size());
	for (const WtCellVertex &vertex : source.vertices) {
		if (!is_finite(vertex.position) || !is_finite(vertex.normal)) {
			return WtRenderBuildStatus::InvalidMesh;
		}
		output.vertices.push_back({ vertex.position, vertex.normal, vertex.material });
	}
	for (std::size_t triangle = 0; triangle < source.indices.size(); triangle += 3) {
		const std::uint32_t a = source.indices[triangle];
		const std::uint32_t b = source.indices[triangle + 1];
		const std::uint32_t c = source.indices[triangle + 2];
		if (a >= source.vertices.size() || b >= source.vertices.size() ||
			c >= source.vertices.size() || triangle_area_squared(
				source.vertices[a].position,
				source.vertices[b].position,
				source.vertices[c].position
			) == 0.0) {
			return WtRenderBuildStatus::InvalidMesh;
		}
		output.indices.push_back(base + a);
		output.indices.push_back(base + b);
		output.indices.push_back(base + c);
	}
	return WtRenderBuildStatus::Ok;
}

} // namespace

WtRenderPayload::WtRenderPayload() {
}

void WtRenderPayload::clear() noexcept {
	key = {};
	generation = {};
	world_origin = {};
	vertices.clear();
	indices.clear();
}

WtRenderBuildStatus wt_build_render_payload(
	const WtChunkMeshResult &mesh,
	WtGenerationToken generation,
	WtRenderPayload &output
) {
	output.clear();
	if (!wt_is_valid_chunk_key(mesh.key) || generation.value == 0 ||
		mesh.world_origin != wt_chunk_bounds(mesh.key).minimum ||
		(mesh.transition_mask & 0xC0U) != 0 ||
		(mesh.transition_mask != 0 && mesh.key.lod == 0)) {
		return WtRenderBuildStatus::InvalidInput;
	}
	output.key = mesh.key;
	output.generation = generation;
	output.world_origin = mesh.world_origin;
	std::size_t expected_vertices = mesh.regular.vertices.size();
	std::size_t expected_indices = mesh.regular.indices.size();
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		if ((mesh.transition_mask & wt_face_bit(
			static_cast<WtChunkFace>(face_index))) != 0) {
			const WtChunkMeshBuffer &transition = mesh.transitions[face_index];
			if (transition.vertices.size() > kWtMaximumRenderVertices - expected_vertices ||
				transition.indices.size() > kWtMaximumRenderIndices - expected_indices) {
				output.clear();
				return WtRenderBuildStatus::CapacityExceeded;
			}
			expected_vertices += transition.vertices.size();
			expected_indices += transition.indices.size();
		}
	}
	if (expected_vertices > kWtMaximumRenderVertices ||
		expected_indices > kWtMaximumRenderIndices) {
		output.clear();
		return WtRenderBuildStatus::CapacityExceeded;
	}
	output.vertices.reserve(expected_vertices);
	output.indices.reserve(expected_indices);
	WtRenderBuildStatus status = append_buffer(mesh.regular, output);
	for (unsigned int face_index = 0;
		status == WtRenderBuildStatus::Ok && face_index < 6;
		++face_index) {
		const bool active = (mesh.transition_mask & wt_face_bit(
			static_cast<WtChunkFace>(face_index))) != 0;
		if (active) {
			status = append_buffer(mesh.transitions[face_index], output);
		} else if (!mesh.transitions[face_index].vertices.empty() ||
			!mesh.transitions[face_index].indices.empty()) {
			status = WtRenderBuildStatus::InvalidMesh;
		}
	}
	if (status != WtRenderBuildStatus::Ok) {
		output.clear();
	}
	return status;
}

} // namespace world_transvoxel
