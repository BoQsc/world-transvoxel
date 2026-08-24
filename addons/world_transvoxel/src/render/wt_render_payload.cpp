#include "render/wt_render_payload.h"

#include "meshing/wt_chunk_mesh_finalize.h"
#include "meshing/wt_chunk_mesh_geometry.h"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>

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

std::uint32_t float_bits(float value) noexcept {
	std::uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

struct RenderVertexKey {
	std::uint32_t position_x = 0;
	std::uint32_t position_y = 0;
	std::uint32_t position_z = 0;
	std::uint32_t normal_x = 0;
	std::uint32_t normal_y = 0;
	std::uint32_t normal_z = 0;
	std::uint16_t material = 0;
	bool material_authored = false;

	bool operator==(const RenderVertexKey &other) const noexcept {
		return position_x == other.position_x &&
			position_y == other.position_y &&
			position_z == other.position_z &&
			normal_x == other.normal_x &&
			normal_y == other.normal_y &&
			normal_z == other.normal_z &&
			material == other.material &&
			material_authored == other.material_authored;
	}
};

struct RenderVertexKeyHash {
	std::size_t operator()(const RenderVertexKey &key) const noexcept {
		std::size_t hash = static_cast<std::size_t>(key.position_x);
		const std::uint32_t values[] = {
			key.position_y,
			key.position_z,
			key.normal_x,
			key.normal_y,
			key.normal_z,
			static_cast<std::uint32_t>(key.material),
			key.material_authored ? 1U : 0U,
		};
		for (const std::uint32_t value : values) {
			hash ^= static_cast<std::size_t>(value) + 0x9e3779b9U +
				(hash << 6U) + (hash >> 2U);
		}
		return hash;
	}
};

RenderVertexKey make_vertex_key(const WtCellVertex &vertex) noexcept {
	return {
		float_bits(vertex.position.x),
		float_bits(vertex.position.y),
		float_bits(vertex.position.z),
		float_bits(vertex.normal.x),
		float_bits(vertex.normal.y),
		float_bits(vertex.normal.z),
		vertex.material,
		vertex.material_authored,
	};
}

WtCellVertex make_render_variant_vertex(
	const WtCellVertex &vertex,
	const WtChunkMeshResult &mesh,
	std::uint8_t transition_mask,
	int primary_transition_face
) noexcept {
	if (!vertex.canonical_position_valid) {
		return vertex;
	}
	WtCellVertex result = vertex;
	const float cell_size = static_cast<float>(wt_lod_cell_size(mesh.key.lod));
	const float extent = static_cast<float>(wt_chunk_extent(mesh.key.lod));
	const float width = cell_size * mesh.transition_width_ratio;
	result.position = wt_snap_chunk_position(wt_deform_chunk_position(
		vertex.canonical_position,
		vertex.normal,
		transition_mask,
		cell_size,
		width,
		extent,
		primary_transition_face
	));
	return result;
}

WtRenderBuildStatus append_buffer(
	const WtChunkMeshBuffer &source,
	WtRenderPayload &output,
	std::unordered_map<RenderVertexKey, std::uint32_t, RenderVertexKeyHash> &vertices
) {
	if ((source.indices.size() % 3U) != 0) {
		return WtRenderBuildStatus::InvalidMesh;
	}
	if (source.vertices.size() > kWtMaximumRenderVertices - output.vertices.size() ||
		source.indices.size() > kWtMaximumRenderIndices - output.indices.size()) {
		return WtRenderBuildStatus::CapacityExceeded;
	}
	std::vector<std::uint32_t> remap;
	remap.reserve(source.vertices.size());
	for (const WtCellVertex &vertex : source.vertices) {
		if (!is_finite(vertex.position) || !is_finite(vertex.normal)) {
			return WtRenderBuildStatus::InvalidMesh;
		}
		const RenderVertexKey key = make_vertex_key(vertex);
		const auto found = vertices.find(key);
		if (found != vertices.end()) {
			remap.push_back(found->second);
			continue;
		}
		if (output.vertices.size() >= kWtMaximumRenderVertices) {
			return WtRenderBuildStatus::CapacityExceeded;
		}
		const std::uint32_t index =
			static_cast<std::uint32_t>(output.vertices.size());
		output.vertices.push_back({
			vertex.position,
			vertex.normal,
			vertex.material,
			vertex.material_authored,
		});
		vertices.emplace(key, index);
		remap.push_back(index);
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
		output.indices.push_back(remap[a]);
		output.indices.push_back(remap[b]);
		output.indices.push_back(remap[c]);
	}
	return WtRenderBuildStatus::Ok;
}

WtRenderBuildStatus append_buffer_variant(
	const WtChunkMeshBuffer &source,
	const WtChunkMeshResult &mesh,
	std::uint8_t transition_mask,
	int primary_transition_face,
	WtChunkMeshBuffer &combined
) {
	if ((source.indices.size() % 3U) != 0) {
		return WtRenderBuildStatus::InvalidMesh;
	}
	if (source.vertices.size() > combined.vertex_limit - combined.vertices.size() ||
		source.indices.size() > combined.index_limit - combined.indices.size()) {
		return WtRenderBuildStatus::CapacityExceeded;
	}
	const std::uint32_t base_index =
		static_cast<std::uint32_t>(combined.vertices.size());
	combined.vertices.reserve(combined.vertices.size() + source.vertices.size());
	for (const WtCellVertex &vertex : source.vertices) {
		combined.vertices.push_back(
			make_render_variant_vertex(
				vertex,
				mesh,
				transition_mask,
				primary_transition_face
			)
		);
	}
	for (const std::uint32_t index : source.indices) {
		if (index >= source.vertices.size()) {
			return WtRenderBuildStatus::InvalidMesh;
		}
		combined.indices.push_back(base_index + index);
	}
	return WtRenderBuildStatus::Ok;
}

WtRenderBuildStatus append_combined_buffer(
	const WtChunkMeshBuffer &source,
	WtChunkMeshBuffer &combined
) {
	if ((source.indices.size() % 3U) != 0) {
		return WtRenderBuildStatus::InvalidMesh;
	}
	if (source.vertices.size() > combined.vertex_limit - combined.vertices.size() ||
		source.indices.size() > combined.index_limit - combined.indices.size()) {
		return WtRenderBuildStatus::CapacityExceeded;
	}
	const std::uint32_t base_index =
		static_cast<std::uint32_t>(combined.vertices.size());
	combined.vertices.insert(
		combined.vertices.end(),
		source.vertices.begin(),
		source.vertices.end()
	);
	for (const std::uint32_t index : source.indices) {
		if (index >= source.vertices.size()) {
			return WtRenderBuildStatus::InvalidMesh;
		}
		combined.indices.push_back(base_index + index);
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
	transition_mask = 0;
	publication_source = WtRenderPublicationSource::CpuAuthority;
	vertices.clear();
	indices.clear();
	water_vertices.clear();
	water_indices.clear();
}

WtRenderBuildStatus wt_build_render_payload(
	const WtChunkMeshResult &mesh,
	WtGenerationToken generation,
	WtRenderPayload &output
) {
	return wt_build_render_payload(
		mesh,
		generation,
		mesh.transition_mask,
		output
	);
}

WtRenderBuildStatus wt_build_render_payload(
	const WtChunkMeshResult &mesh,
	WtGenerationToken generation,
	std::uint8_t transition_mask,
	WtRenderPayload &output
) {
	output.clear();
	const std::uint8_t cached_transition_mask =
		mesh.cached_transition_mask == 0 ? mesh.transition_mask :
			mesh.cached_transition_mask;
	if (!wt_is_valid_chunk_key(mesh.key) || generation.value == 0 ||
		mesh.world_origin != wt_chunk_bounds(mesh.key).minimum ||
		(mesh.transition_mask & 0xC0U) != 0 ||
		(cached_transition_mask & 0xC0U) != 0 ||
		(transition_mask & 0xC0U) != 0 ||
		transition_mask != mesh.transition_mask ||
		(mesh.transition_mask & static_cast<std::uint8_t>(~cached_transition_mask)) != 0 ||
		(transition_mask & static_cast<std::uint8_t>(~cached_transition_mask)) != 0 ||
		!std::isfinite(mesh.transition_width_ratio) ||
		mesh.transition_width_ratio <= 0.0F ||
		mesh.transition_width_ratio >= 1.0F ||
		((mesh.transition_mask != 0 || cached_transition_mask != 0 ||
				transition_mask != 0) && mesh.key.lod == 0)) {
		return WtRenderBuildStatus::InvalidInput;
	}
	output.key = mesh.key;
	output.generation = generation;
	output.world_origin = mesh.world_origin;
	output.transition_mask = transition_mask;
	std::size_t expected_vertices = mesh.regular.vertices.size();
	std::size_t expected_indices = mesh.regular.indices.size();
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		if ((transition_mask & wt_face_bit(
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
	WtChunkMeshBuffer combined;
	combined.vertex_limit = expected_vertices;
	combined.index_limit = expected_indices;
	combined.vertices.reserve(expected_vertices);
	combined.indices.reserve(expected_indices);
	WtRenderBuildStatus status = append_buffer_variant(
		mesh.regular,
		mesh,
		transition_mask,
		-1,
		combined
	);
	for (unsigned int face_index = 0;
		status == WtRenderBuildStatus::Ok && face_index < 6;
		++face_index) {
		const bool active = (transition_mask & wt_face_bit(
			static_cast<WtChunkFace>(face_index))) != 0;
		if (active) {
			status = append_buffer_variant(
				mesh.transitions[face_index],
				mesh,
				transition_mask,
				static_cast<int>(face_index),
				combined
			);
		} else if ((cached_transition_mask & wt_face_bit(
				static_cast<WtChunkFace>(face_index))) == 0 &&
			(!mesh.transitions[face_index].vertices.empty() ||
				!mesh.transitions[face_index].indices.empty())) {
			status = WtRenderBuildStatus::InvalidMesh;
		}
	}
	if (status == WtRenderBuildStatus::Ok) {
		wt_finalize_deformed_triangles(combined);
		output.vertices.reserve(combined.vertices.size());
		output.indices.reserve(combined.indices.size());
		std::unordered_map<RenderVertexKey, std::uint32_t, RenderVertexKeyHash> vertices;
		vertices.reserve(combined.vertices.size());
		status = append_buffer(combined, output, vertices);
	}
	if (status != WtRenderBuildStatus::Ok) {
		output.clear();
	}
	return status;
}

WtRenderBuildStatus wt_build_render_payload(
	const WtChunkMeshResult &mesh,
	const WtChunkMeshResult &water_mesh,
	WtGenerationToken generation,
	WtRenderPayload &output
) {
	return wt_build_render_payload(
		mesh,
		water_mesh,
		generation,
		mesh.transition_mask,
		output
	);
}

WtRenderBuildStatus wt_build_render_payload(
	const WtChunkMeshResult &mesh,
	const WtChunkMeshResult &water_mesh,
	WtGenerationToken generation,
	std::uint8_t transition_mask,
	WtRenderPayload &output
) {
	if (water_mesh.key != mesh.key ||
		water_mesh.world_origin != mesh.world_origin ||
		water_mesh.transition_mask != mesh.transition_mask ||
		water_mesh.cached_transition_mask != mesh.cached_transition_mask ||
		water_mesh.transition_width_ratio != mesh.transition_width_ratio) {
		output.clear();
		return WtRenderBuildStatus::InvalidInput;
	}
	WtRenderBuildStatus status = wt_build_render_payload(
		mesh,
		generation,
		transition_mask,
		output
	);
	if (status != WtRenderBuildStatus::Ok) {
		return status;
	}
	WtRenderPayload water;
	status = wt_build_render_payload(water_mesh, generation, transition_mask, water);
	if (status != WtRenderBuildStatus::Ok) {
		output.clear();
		return status;
	}
	output.water_vertices = std::move(water.vertices);
	output.water_indices = std::move(water.indices);
	return WtRenderBuildStatus::Ok;
}

} // namespace world_transvoxel
