#include "meshing/wt_chunk_mesh_finalize.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace world_transvoxel {

WtVec3 wt_interpolated_mesh_normal(
	const WtCellSample &a,
	const WtCellSample &b,
	float isovalue
) noexcept {
	const float alpha = std::max(
		0.0F,
		std::min(1.0F, (isovalue - a.density) / (b.density - a.density))
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
	std::size_t write_index = 0;
	for (std::size_t index = 0; index < buffer.indices.size(); index += 3) {
		const std::uint32_t index_a = buffer.indices[index];
		std::uint32_t index_b = buffer.indices[index + 1];
		std::uint32_t index_c = buffer.indices[index + 2];
		const WtVec3 &a = buffer.vertices[index_a].position;
		const WtVec3 &b = buffer.vertices[index_b].position;
		const WtVec3 &c = buffer.vertices[index_c].position;
		const double ab_x = static_cast<double>(b.x) - a.x;
		const double ab_y = static_cast<double>(b.y) - a.y;
		const double ab_z = static_cast<double>(b.z) - a.z;
		const double ac_x = static_cast<double>(c.x) - a.x;
		const double ac_y = static_cast<double>(c.y) - a.y;
		const double ac_z = static_cast<double>(c.z) - a.z;
		const double cross_x = ab_y * ac_z - ab_z * ac_y;
		const double cross_y = ab_z * ac_x - ab_x * ac_z;
		const double cross_z = ab_x * ac_y - ab_y * ac_x;
		if (cross_x * cross_x + cross_y * cross_y + cross_z * cross_z == 0.0) {
			continue;
		}
		const WtVec3 &normal_a = buffer.vertices[index_a].normal;
		const WtVec3 &normal_b = buffer.vertices[index_b].normal;
		const WtVec3 &normal_c = buffer.vertices[index_c].normal;
		const double outward_dot =
			cross_x * static_cast<double>(
				normal_a.x + normal_b.x + normal_c.x
			) +
			cross_y * static_cast<double>(
				normal_a.y + normal_b.y + normal_c.y
			) +
			cross_z * static_cast<double>(
				normal_a.z + normal_b.z + normal_c.z
			);
		if (outward_dot < 0.0) {
			std::swap(index_b, index_c);
		}
		buffer.indices[write_index++] = index_a;
		buffer.indices[write_index++] = index_b;
		buffer.indices[write_index++] = index_c;
	}
	buffer.indices.resize(write_index);

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

}
