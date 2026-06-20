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
	const double denominator =
		static_cast<double>(sample_b.density) - static_cast<double>(sample_a.density);
	double alpha =
		(static_cast<double>(isovalue) - static_cast<double>(sample_a.density)) /
		denominator;
	alpha = std::max(0.0, std::min(1.0, alpha));
	const WtVec3 &a = endpoint_positions[vertex.endpoint_a];
	const WtVec3 &b = endpoint_positions[vertex.endpoint_b];
	constexpr double scale = 65536.0;
	const double local_x =
		static_cast<double>(a.x) * (1.0 - alpha) + static_cast<double>(b.x) * alpha;
	const double local_y =
		static_cast<double>(a.y) * (1.0 - alpha) + static_cast<double>(b.y) * alpha;
	const double local_z =
		static_cast<double>(a.z) * (1.0 - alpha) + static_cast<double>(b.z) * alpha;
	return {
		static_cast<float>(std::round(local_x * scale) / scale),
		static_cast<float>(std::round(local_y * scale) / scale),
		static_cast<float>(std::round(local_z * scale) / scale),
	};
}

WtVec3 wt_deform_chunk_position(
	WtVec3 position,
	std::uint8_t transition_mask,
	float cell_size,
	float width,
	float extent,
	int primary_transition_face
) noexcept {
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
		transition_factor = std::max(0.0F, std::min(1.0F, primary_distance / width));
	}
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		const WtChunkFace face = static_cast<WtChunkFace>(face_index);
		if ((transition_mask & wt_face_bit(face)) == 0 ||
			static_cast<int>(face_index) == primary_transition_face) {
			continue;
		}
		float distance = 0.0F;
		WtIntegerVector inward{};
		face_distance_and_inward(position, face, extent, distance, inward);
		if (distance >= 0.0F && distance < cell_size) {
			position = add(position, inward,
				transition_factor * width * (1.0F - distance / cell_size));
		}
	}
	return position;
}

WtVec3 wt_snap_chunk_position(WtVec3 position) noexcept {
	constexpr float scale = 65536.0F;
	position.x = std::round(position.x * scale) / scale;
	position.y = std::round(position.y * scale) / scale;
	position.z = std::round(position.z * scale) / scale;
	return position;
}

} // namespace world_transvoxel
