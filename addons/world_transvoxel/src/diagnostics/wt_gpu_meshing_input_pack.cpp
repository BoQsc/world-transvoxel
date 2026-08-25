#include "diagnostics/wt_gpu_meshing_input_pack.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "core/wt_chunk_key.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace world_transvoxel {
namespace {

bool finite(float value) noexcept {
	return std::isfinite(value);
}

bool finite(const WtVec3 &value) noexcept {
	return finite(value.x) && finite(value.y) && finite(value.z);
}

std::int32_t i32_bits(std::uint32_t value) noexcept {
	std::int32_t result = 0;
	static_assert(sizeof(result) == sizeof(value));
	std::memcpy(&result, &value, sizeof(result));
	return result;
}

void append_sample(
	const WtCellSample &sample,
	std::int32_t sample_index,
	WtGpuMeshingInputPack &output
) {
	output.field_values.insert(output.field_values.end(), {
		sample.density,
		sample.gradient.x,
		sample.gradient.y,
		sample.gradient.z,
	});
	output.field_meta.insert(output.field_meta.end(), {
		static_cast<std::int32_t>(sample.material),
		sample.material_authored ? 1 : 0,
		sample_index,
		1,
	});
	output.sample_references.push_back(sample_index);
}

template <typename T>
std::size_t byte_size(const std::vector<T> &values) noexcept {
	return values.size() * sizeof(T);
}

void expand_bounds(const WtVec3 &point, WtGpuMeshingInputPack &output) {
	output.bounds_min.x = std::min(output.bounds_min.x, point.x);
	output.bounds_min.y = std::min(output.bounds_min.y, point.y);
	output.bounds_min.z = std::min(output.bounds_min.z, point.z);
	output.bounds_max.x = std::max(output.bounds_max.x, point.x);
	output.bounds_max.y = std::max(output.bounds_max.y, point.y);
	output.bounds_max.z = std::max(output.bounds_max.z, point.z);
}

WtVec3 offset_point(
	const WtVec3 &origin,
	const WtVec3 &axis_u,
	const WtVec3 &axis_v,
	const WtVec3 &axis_w,
	float u,
	float v,
	float w
) noexcept {
	return {
		origin.x + axis_u.x * u + axis_v.x * v + axis_w.x * w,
		origin.y + axis_u.y * u + axis_v.y * v + axis_w.y * w,
		origin.z + axis_u.z * u + axis_v.z * v + axis_w.z * w,
	};
}

void transition_basis(
	std::int32_t orientation,
	WtVec3 &axis_u,
	WtVec3 &axis_v,
	WtVec3 &axis_w
) noexcept {
	switch (orientation) {
	case 0:
		axis_u = { 0.0F, 1.0F, 0.0F };
		axis_v = { 0.0F, 0.0F, 1.0F };
		axis_w = { 1.0F, 0.0F, 0.0F };
		break;
	case 1:
		axis_u = { 0.0F, 1.0F, 0.0F };
		axis_v = { 0.0F, 0.0F, -1.0F };
		axis_w = { -1.0F, 0.0F, 0.0F };
		break;
	case 2:
		axis_u = { 0.0F, 0.0F, 1.0F };
		axis_v = { 1.0F, 0.0F, 0.0F };
		axis_w = { 0.0F, 1.0F, 0.0F };
		break;
	case 3:
		axis_u = { 0.0F, 0.0F, 1.0F };
		axis_v = { -1.0F, 0.0F, 0.0F };
		axis_w = { 0.0F, -1.0F, 0.0F };
		break;
	case 4:
		axis_u = { 1.0F, 0.0F, 0.0F };
		axis_v = { 0.0F, 1.0F, 0.0F };
		axis_w = { 0.0F, 0.0F, 1.0F };
		break;
	default:
		axis_u = { 1.0F, 0.0F, 0.0F };
		axis_v = { 0.0F, -1.0F, 0.0F };
		axis_w = { 0.0F, 0.0F, -1.0F };
		break;
	}
}

void expand_cell_bounds(
	const WtVec3 &origin,
	float spacing,
	bool transition,
	std::int32_t orientation,
	float transition_width,
	WtGpuMeshingInputPack &output
) {
	if (!transition) {
		expand_bounds(origin, output);
		expand_bounds({
			origin.x + spacing,
			origin.y + spacing,
			origin.z + spacing,
		}, output);
		return;
	}
	WtVec3 axis_u;
	WtVec3 axis_v;
	WtVec3 axis_w;
	transition_basis(orientation, axis_u, axis_v, axis_w);
	for (const float u : { 0.0F, 2.0F * spacing }) {
		for (const float v : { 0.0F, 2.0F * spacing }) {
			for (const float w : { 0.0F, transition_width }) {
				expand_bounds(offset_point(
					origin, axis_u, axis_v, axis_w, u, v, w
				), output);
			}
		}
	}
}

} // namespace

bool wt_pack_gpu_meshing_input(
	const WtGpuMeshingShadowRequest &request,
	WtGpuMeshingInputPack &output,
	std::string &error
) {
	output = {};
	error.clear();
	if (request.records.empty() ||
		request.records.size() > kWtMaximumRecordedChunkCells) {
		error = "GPU meshing record count is outside the bounded contract";
		return false;
	}
	if (!wt_is_valid_chunk_key(request.job.key)) {
		error = "GPU meshing chunk key is invalid";
		return false;
	}
	const WtGridPoint chunk_minimum = wt_chunk_bounds(request.job.key).minimum;
	const WtVec3 world_origin = {
		static_cast<float>(chunk_minimum.x),
		static_cast<float>(chunk_minimum.y),
		static_cast<float>(chunk_minimum.z),
	};
	const std::size_t maximum_samples =
		request.records.size() * kWtTransitionSampleCount;
	if (maximum_samples > static_cast<std::size_t>(
			std::numeric_limits<std::int32_t>::max()
		)) {
		error = "GPU meshing sample count exceeds the 32-bit shader contract";
		return false;
	}
	output.cell_count = request.records.size();
	output.field_values.reserve(maximum_samples * 4U);
	output.field_meta.reserve(maximum_samples * 4U);
	output.cell_headers.reserve(output.cell_count * 4U);
	output.cell_origins.reserve(output.cell_count * 4U);
	output.cell_options.reserve(output.cell_count * 4U);
	output.sample_references.reserve(maximum_samples);
	const float infinity = std::numeric_limits<float>::infinity();
	output.bounds_min = { infinity, infinity, infinity };
	output.bounds_max = { -infinity, -infinity, -infinity };

	for (const WtRecordedMeshingCell &record : request.records) {
		const bool transition = record.type == WtRecordedCellType::Transition;
		const unsigned int sample_count = transition ?
			kWtTransitionSampleCount : kWtRegularSampleCount;
		const WtVec3 local_origin = transition ?
			record.transition_input.full_resolution_origin :
			record.regular_input.origin;
		const WtVec3 origin = {
			world_origin.x + local_origin.x,
			world_origin.y + local_origin.y,
			world_origin.z + local_origin.z,
		};
		const float spacing = transition ?
			record.transition_input.sample_spacing :
			record.regular_input.cell_size;
		const float transition_width = transition ?
			record.transition_input.transition_width : 0.0F;
		const float isovalue = transition ?
			record.transition_input.isovalue : record.regular_input.isovalue;
		const std::int32_t orientation = transition ?
			static_cast<std::int32_t>(record.transition_input.orientation) : 0;
		if (!finite(origin) || !finite(spacing) || spacing <= 0.0F ||
			!finite(isovalue) || (transition && (
				orientation < 0 || orientation > 5 ||
				!finite(transition_width) || transition_width <= 0.0F
			))) {
			error = "GPU meshing cell transform is invalid";
			return false;
		}
		const std::int32_t reference_offset = static_cast<std::int32_t>(
			output.sample_references.size()
		);
		output.cell_headers.insert(output.cell_headers.end(), {
			transition ? 1 : 0,
			orientation,
			reference_offset,
			static_cast<std::int32_t>(sample_count),
		});
		output.cell_origins.insert(output.cell_origins.end(), {
			origin.x, origin.y, origin.z, spacing,
		});
		output.cell_options.insert(output.cell_options.end(), {
			transition_width, isovalue, 0.0F, 0.0F,
		});
		expand_cell_bounds(
			origin,
			spacing,
			transition,
			orientation,
			transition_width,
			output
		);
		for (unsigned int index = 0; index < sample_count; ++index) {
			const WtCellSample &sample = transition ?
				record.transition_input.samples[index] :
				record.regular_input.samples[index];
			if (!finite(sample.density) || !finite(sample.gradient)) {
				error = "GPU meshing sample is non-finite";
				return false;
			}
			append_sample(
				sample,
				static_cast<std::int32_t>(output.sample_count),
				output
			);
			++output.sample_count;
		}
	}

	const std::uint64_t source_revision = request.job.source_revision;
	const std::uint64_t world_revision = request.job.world_revision;
	output.config = {
		static_cast<std::int32_t>(output.cell_count),
		static_cast<std::int32_t>(output.sample_count),
		static_cast<std::int32_t>(output.sample_references.size()),
		0,
		request.job.key.x,
		request.job.key.y,
		request.job.key.z,
		static_cast<std::int32_t>(request.job.key.lod),
		i32_bits(static_cast<std::uint32_t>(request.job.generation.value)),
		i32_bits(static_cast<std::uint32_t>(source_revision)),
		i32_bits(static_cast<std::uint32_t>(source_revision >> 32U)),
		i32_bits(static_cast<std::uint32_t>(world_revision)),
		i32_bits(static_cast<std::uint32_t>(world_revision >> 32U)),
		static_cast<std::int32_t>(request.transition_mask),
		request.surface == WtGpuMeshingShadowSurface::StaticWater ? 1 : 0,
		static_cast<std::int32_t>(output.sample_count),
	};

	const WtTransvoxelTablePack &tables = wt_get_transvoxel_mit_table_pack();
	output.packed_byte_count =
		byte_size(output.field_values) + byte_size(output.field_meta) +
		byte_size(output.cell_headers) + byte_size(output.cell_origins) +
		byte_size(output.cell_options) + byte_size(output.sample_references) +
		sizeof(output.config) +
		sizeof(tables.regular_cell_class) +
		sizeof(tables.regular_cell_data) +
		sizeof(tables.regular_vertex_data) +
		sizeof(tables.transition_cell_class) +
		sizeof(tables.transition_cell_data) +
		sizeof(tables.transition_vertex_data);
	return true;
}

} // namespace world_transvoxel
