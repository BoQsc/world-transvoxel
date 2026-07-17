#include "storage/wt_chunk_surface_shift.h"

#include "meshing/wt_multiresolution_vertex_resolver.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace world_transvoxel {
namespace {

constexpr std::uint16_t kAxisEdgeCount = static_cast<std::uint16_t>(
	kWtChunkCellsPerAxis *
	(kWtChunkCellsPerAxis + 1) *
	(kWtChunkCellsPerAxis + 1)
);

bool valid_metadata(const WtChunkPageMetadata &metadata) noexcept {
	return wt_is_valid_chunk_key(metadata.key) &&
		metadata.cell_spacing == static_cast<std::uint64_t>(
			wt_lod_cell_size(metadata.key.lod)
		);
}

bool to_lattice_coordinate(
	std::int64_t coordinate,
	std::int64_t minimum,
	std::int64_t spacing,
	int &output
) noexcept {
	const std::int64_t difference = coordinate - minimum;
	if ((difference % spacing) != 0) {
		return false;
	}
	const std::int64_t value = difference / spacing;
	if (value < 0 || value > kWtChunkCellsPerAxis) {
		return false;
	}
	output = static_cast<int>(value);
	return true;
}

bool decode_edge_index(
	std::uint16_t edge_index,
	int &axis,
	int &x,
	int &y,
	int &z
) noexcept {
	if (edge_index >= kWtChunkSurfaceEdgeCount) {
		return false;
	}
	axis = edge_index / kAxisEdgeCount;
	std::uint16_t local = static_cast<std::uint16_t>(
		edge_index % kAxisEdgeCount
	);
	if (axis == 0) {
		x = local % kWtChunkCellsPerAxis;
		local /= kWtChunkCellsPerAxis;
		y = local % (kWtChunkCellsPerAxis + 1);
		z = local / (kWtChunkCellsPerAxis + 1);
	} else if (axis == 1) {
		x = local % (kWtChunkCellsPerAxis + 1);
		local /= kWtChunkCellsPerAxis + 1;
		y = local % kWtChunkCellsPerAxis;
		z = local / kWtChunkCellsPerAxis;
	} else {
		x = local % (kWtChunkCellsPerAxis + 1);
		local /= kWtChunkCellsPerAxis + 1;
		y = local % (kWtChunkCellsPerAxis + 1);
		z = local / (kWtChunkCellsPerAxis + 1);
	}
	return true;
}

bool finite_sample(
	const WtCellSample &sample
) noexcept {
	return std::isfinite(sample.density) &&
		std::isfinite(sample.gradient.x) &&
		std::isfinite(sample.gradient.y) &&
		std::isfinite(sample.gradient.z);
}

} // namespace

bool wt_chunk_surface_edge_index(
	const WtChunkPageMetadata &metadata,
	const WtGridPoint &endpoint_a,
	const WtGridPoint &endpoint_b,
	std::uint16_t &edge_index,
	bool &reversed
) noexcept {
	edge_index = 0;
	reversed = false;
	if (!valid_metadata(metadata)) {
		return false;
	}
	const std::int64_t spacing = static_cast<std::int64_t>(
		metadata.cell_spacing
	);
	const std::int64_t differences[3] = {
		endpoint_b.x - endpoint_a.x,
		endpoint_b.y - endpoint_a.y,
		endpoint_b.z - endpoint_a.z,
	};
	int axis = -1;
	for (int candidate = 0; candidate < 3; ++candidate) {
		if (differences[candidate] == 0) {
			continue;
		}
		if (axis >= 0 || std::abs(differences[candidate]) != spacing) {
			return false;
		}
		axis = candidate;
	}
	if (axis < 0) {
		return false;
	}
	reversed = differences[axis] < 0;
	const WtGridPoint &start = reversed ? endpoint_b : endpoint_a;
	const WtGridPoint minimum = wt_chunk_bounds(metadata.key).minimum;
	int coordinate[3]{};
	if (!to_lattice_coordinate(start.x, minimum.x, spacing, coordinate[0]) ||
		!to_lattice_coordinate(start.y, minimum.y, spacing, coordinate[1]) ||
		!to_lattice_coordinate(start.z, minimum.z, spacing, coordinate[2]) ||
		coordinate[axis] >= kWtChunkCellsPerAxis) {
		return false;
	}
	std::uint32_t local = 0;
	if (axis == 0) {
		local = static_cast<std::uint32_t>(
			(coordinate[2] * (kWtChunkCellsPerAxis + 1) + coordinate[1]) *
			kWtChunkCellsPerAxis + coordinate[0]
		);
	} else if (axis == 1) {
		local = static_cast<std::uint32_t>(
			(coordinate[2] * kWtChunkCellsPerAxis + coordinate[1]) *
			(kWtChunkCellsPerAxis + 1) + coordinate[0]
		);
	} else {
		local = static_cast<std::uint32_t>(
			(coordinate[2] * (kWtChunkCellsPerAxis + 1) + coordinate[1]) *
			(kWtChunkCellsPerAxis + 1) + coordinate[0]
		);
	}
	const std::uint32_t index =
		static_cast<std::uint32_t>(axis) * kAxisEdgeCount + local;
	if (index >= kWtChunkSurfaceEdgeCount ||
		index > std::numeric_limits<std::uint16_t>::max()) {
		return false;
	}
	edge_index = static_cast<std::uint16_t>(index);
	return true;
}

bool wt_chunk_surface_edge_points(
	const WtChunkPageMetadata &metadata,
	std::uint16_t edge_index,
	WtGridPoint &endpoint_a,
	WtGridPoint &endpoint_b
) noexcept {
	endpoint_a = {};
	endpoint_b = {};
	if (!valid_metadata(metadata)) {
		return false;
	}
	int axis = 0;
	int x = 0;
	int y = 0;
	int z = 0;
	if (!decode_edge_index(edge_index, axis, x, y, z)) {
		return false;
	}
	const std::int64_t spacing = static_cast<std::int64_t>(
		metadata.cell_spacing
	);
	const WtGridPoint minimum = wt_chunk_bounds(metadata.key).minimum;
	endpoint_a = {
		minimum.x + static_cast<std::int64_t>(x) * spacing,
		minimum.y + static_cast<std::int64_t>(y) * spacing,
		minimum.z + static_cast<std::int64_t>(z) * spacing,
	};
	endpoint_b = endpoint_a;
	if (axis == 0) endpoint_b.x += spacing;
	if (axis == 1) endpoint_b.y += spacing;
	if (axis == 2) endpoint_b.z += spacing;
	return true;
}

bool wt_resolve_chunk_surface_shift_record(
	const WtChunkPage &page,
	const WtGridPoint &endpoint_a,
	const WtGridPoint &endpoint_b,
	float isovalue,
	WtResolvedMultiresolutionEdge &output
) noexcept {
	output = {};
	if (!page.surface_shift_valid ||
		!std::isfinite(isovalue) ||
		isovalue != page.surface_shift_isovalue) {
		return false;
	}
	std::uint16_t edge_index = 0;
	bool reversed = false;
	if (!wt_chunk_surface_edge_index(
			page.metadata,
			endpoint_a,
			endpoint_b,
			edge_index,
			reversed
		)) {
		return false;
	}
	const auto record = std::lower_bound(
		page.surface_shift_records.begin(),
		page.surface_shift_records.end(),
		edge_index,
		[](const WtChunkSurfaceShiftRecord &left, std::uint16_t right) {
			return left.edge_index < right;
		}
	);
	if (record == page.surface_shift_records.end() ||
		record->edge_index != edge_index) {
		return false;
	}
	WtGridPoint coarse_a;
	WtGridPoint coarse_b;
	if (!wt_chunk_surface_edge_points(
			page.metadata, edge_index, coarse_a, coarse_b
		) ||
		record->unit_offset >= page.metadata.cell_spacing) {
		return false;
	}
	WtGridPoint fine_a = coarse_a;
	WtGridPoint fine_b = coarse_a;
	const std::int64_t offset = static_cast<std::int64_t>(
		record->unit_offset
	);
	if (coarse_a.x != coarse_b.x) {
		fine_a.x += offset;
		fine_b.x = fine_a.x + 1;
	} else if (coarse_a.y != coarse_b.y) {
		fine_a.y += offset;
		fine_b.y = fine_a.y + 1;
	} else {
		fine_a.z += offset;
		fine_b.z = fine_a.z + 1;
	}
	if (reversed) {
		output = {
			fine_b,
			fine_a,
			record->sample_b,
			record->sample_a,
		};
	} else {
		output = {
			fine_a,
			fine_b,
			record->sample_a,
			record->sample_b,
		};
	}
	return finite_sample(output.sample_a) &&
		finite_sample(output.sample_b) &&
		(output.sample_a.density < isovalue) !=
			(output.sample_b.density < isovalue);
}

} // namespace world_transvoxel
