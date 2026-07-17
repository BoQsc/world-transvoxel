#include "meshing/wt_chunk_mesh_geometry.h"

#include "core/wt_chunk_key.h"

#include <algorithm>
#include <cmath>

namespace world_transvoxel {
namespace {

struct WtIntegerVector {
	int x;
	int y;
	int z;
};

WtVec3 add(const WtVec3 &point, const WtIntegerVector &vector, float scale) noexcept {
	return {
		point.x + static_cast<float>(vector.x) * scale,
		point.y + static_cast<float>(vector.y) * scale,
		point.z + static_cast<float>(vector.z) * scale,
	};
}

void face_distance_and_inward(
	const WtVec3 &position,
	WtChunkFace face,
	float extent,
	float &distance,
	WtIntegerVector &inward
) noexcept {
	switch (face) {
		case WtChunkFace::NegativeX: distance = position.x; inward = { 1, 0, 0 }; break;
		case WtChunkFace::PositiveX: distance = extent - position.x; inward = { -1, 0, 0 }; break;
		case WtChunkFace::NegativeY: distance = position.y; inward = { 0, 1, 0 }; break;
		case WtChunkFace::PositiveY: distance = extent - position.y; inward = { 0, -1, 0 }; break;
		case WtChunkFace::NegativeZ: distance = position.z; inward = { 0, 0, 1 }; break;
		case WtChunkFace::PositiveZ: distance = extent - position.z; inward = { 0, 0, -1 }; break;
	}
}

} // namespace

WtVec3 wt_canonical_chunk_position(
	const WtCellVertex &vertex,
	const std::array<WtVec3, kWtTransitionTopologySampleCount> &endpoint_positions,
	const WtCellSample *samples,
	float isovalue
) noexcept {
	const WtCellSample &sample_a = samples[vertex.endpoint_a];
	const WtCellSample &sample_b = samples[vertex.endpoint_b];
	return wt_canonical_edge_position(
		endpoint_positions[vertex.endpoint_a],
		endpoint_positions[vertex.endpoint_b],
		sample_a,
		sample_b,
		isovalue
	);
}

WtVec3 wt_canonical_edge_position(
	const WtVec3 &endpoint_a,
	const WtVec3 &endpoint_b,
	const WtCellSample &sample_a,
	const WtCellSample &sample_b,
	float isovalue
) noexcept {
	const double denominator =
		static_cast<double>(sample_b.density) - static_cast<double>(sample_a.density);
	double alpha =
		(static_cast<double>(isovalue) - static_cast<double>(sample_a.density)) /
		denominator;
	alpha = wt_regularized_isosurface_alpha(alpha);
	constexpr double scale = 65536.0;
	const double local_x =
		static_cast<double>(endpoint_a.x) * (1.0 - alpha) +
		static_cast<double>(endpoint_b.x) * alpha;
	const double local_y =
		static_cast<double>(endpoint_a.y) * (1.0 - alpha) +
		static_cast<double>(endpoint_b.y) * alpha;
	const double local_z =
		static_cast<double>(endpoint_a.z) * (1.0 - alpha) +
		static_cast<double>(endpoint_b.z) * alpha;
	return {
		static_cast<float>(std::round(local_x * scale) / scale),
		static_cast<float>(std::round(local_y * scale) / scale),
		static_cast<float>(std::round(local_z * scale) / scale),
	};
}

std::uint16_t wt_closest_isosurface_endpoint_material(
	const WtCellSample &sample_a,
	const WtCellSample &sample_b,
	float isovalue
) noexcept {
	const float distance_a = std::fabs(sample_a.density - isovalue);
	const float distance_b = std::fabs(sample_b.density - isovalue);
	return distance_a <= distance_b ? sample_a.material : sample_b.material;
}

WtVec3 wt_deform_chunk_position(
	WtVec3 position,
	const WtVec3 &normal,
	std::uint8_t transition_mask,
	float cell_size,
	float width,
	float extent,
	int primary_transition_face
) noexcept {
	WtVec3 primary = position;
	float transition_factor = 1.0F;
	if (primary_transition_face >= 0) {
		float primary_distance = 0.0F;
		WtIntegerVector primary_inward{};
		face_distance_and_inward(
			position,
			static_cast<WtChunkFace>(primary_transition_face),
			extent,
			primary_distance,
			primary_inward
		);
		transition_factor = std::max(
			0.0F,
			std::min(1.0F, primary_distance / width)
		);
		primary = add(position, primary_inward, -primary_distance);
	}

	std::uint8_t near_face_mask = 0;
	if (primary.x < cell_size) near_face_mask |= wt_face_bit(WtChunkFace::NegativeX);
	if (primary.x > extent - cell_size) near_face_mask |= wt_face_bit(WtChunkFace::PositiveX);
	if (primary.y < cell_size) near_face_mask |= wt_face_bit(WtChunkFace::NegativeY);
	if (primary.y > extent - cell_size) near_face_mask |= wt_face_bit(WtChunkFace::PositiveY);
	if (primary.z < cell_size) near_face_mask |= wt_face_bit(WtChunkFace::NegativeZ);
	if (primary.z > extent - cell_size) near_face_mask |= wt_face_bit(WtChunkFace::PositiveZ);

	std::uint8_t vertex_border_mask = 0;
	if (primary.x == 0.0F) vertex_border_mask |= wt_face_bit(WtChunkFace::NegativeX);
	if (primary.x == extent) vertex_border_mask |= wt_face_bit(WtChunkFace::PositiveX);
	if (primary.y == 0.0F) vertex_border_mask |= wt_face_bit(WtChunkFace::NegativeY);
	if (primary.y == extent) vertex_border_mask |= wt_face_bit(WtChunkFace::PositiveY);
	if (primary.z == 0.0F) vertex_border_mask |= wt_face_bit(WtChunkFace::NegativeZ);
	if (primary.z == extent) vertex_border_mask |= wt_face_bit(WtChunkFace::PositiveZ);

	const bool has_active_transition = (near_face_mask & transition_mask) != 0;
	const bool touches_same_lod_face =
		(vertex_border_mask & static_cast<std::uint8_t>(~transition_mask)) != 0;
	if (!has_active_transition || touches_same_lod_face) {
		return primary;
	}

	WtVec3 offset;
	if (primary.x < cell_size) {
		offset.x = width * (1.0F - primary.x / cell_size);
	} else if (primary.x > extent - cell_size) {
		offset.x = -width * (1.0F - (extent - primary.x) / cell_size);
	}
	if (primary.y < cell_size) {
		offset.y = width * (1.0F - primary.y / cell_size);
	} else if (primary.y > extent - cell_size) {
		offset.y = -width * (1.0F - (extent - primary.y) / cell_size);
	}
	if (primary.z < cell_size) {
		offset.z = width * (1.0F - primary.z / cell_size);
	} else if (primary.z > extent - cell_size) {
		offset.z = -width * (1.0F - (extent - primary.z) / cell_size);
	}
	const float normal_offset =
		normal.x * offset.x + normal.y * offset.y + normal.z * offset.z;
	const WtVec3 projected = {
		offset.x - normal.x * normal_offset,
		offset.y - normal.y * normal_offset,
		offset.z - normal.z * normal_offset,
	};
	return {
		primary.x + transition_factor * projected.x,
		primary.y + transition_factor * projected.y,
		primary.z + transition_factor * projected.z,
	};
}

WtVec3 wt_snap_chunk_position(WtVec3 position) noexcept {
	constexpr float scale = 65536.0F;
	position.x = std::round(position.x * scale) / scale;
	position.y = std::round(position.y * scale) / scale;
	position.z = std::round(position.z * scale) / scale;
	return position;
}

} // namespace world_transvoxel
