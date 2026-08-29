#include "diagnostics/wt_gpu_meshing_input_pack.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "core/wt_chunk_key.h"
#include "storage/wt_chunk_page.h"

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

bool finite(const WtCellSample &value) noexcept {
	return finite(value.density) && finite(value.gradient);
}

std::int32_t i32_bits(std::uint32_t value) noexcept {
	std::int32_t result = 0;
	static_assert(sizeof(result) == sizeof(value));
	std::memcpy(&result, &value, sizeof(result));
	return result;
}

std::int32_t i32_bits(float value) noexcept {
	std::int32_t result = 0;
	static_assert(sizeof(result) == sizeof(value));
	std::memcpy(&result, &value, sizeof(result));
	return result;
}

void append_packed_field_sample(
	const WtCellSample &sample,
	std::int32_t page_index,
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
		page_index,
		1,
	});
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

unsigned int face_count(std::uint8_t mask) noexcept {
	unsigned int count = 0;
	for (unsigned int face = 0; face < 6; ++face) {
		if ((mask & (1U << face)) != 0) ++count;
	}
	return count;
}

bool valid_page_field_input(
	const WtGpuMeshingShadowPage &retained,
	std::uint64_t source_revision
) noexcept {
	if (!retained.page || retained.key != retained.page->metadata.key) {
		return false;
	}
	const WtChunkPage &page = *retained.page;
	return wt_is_valid_chunk_key(page.metadata.key) &&
		page.metadata.source_revision == source_revision &&
		page.metadata.sample_minimum == -1 &&
		page.metadata.sample_maximum == 17 &&
		page.metadata.dimension_x == kWtChunkMeshingSamplesPerAxis &&
		page.metadata.dimension_y == kWtChunkMeshingSamplesPerAxis &&
		page.metadata.dimension_z == kWtChunkMeshingSamplesPerAxis &&
		page.metadata.sample_count == kWtChunkPageSampleCount &&
		page.metadata.cell_spacing == static_cast<std::uint64_t>(
			wt_lod_cell_size(page.metadata.key.lod)
		) && page.samples.size() == kWtChunkPageSampleCount &&
		page.surface_shift_valid && finite(page.surface_shift_isovalue) &&
		page.surface_shift_isovalue == 0.0F &&
		std::all_of(
			page.surface_shift_records.begin(),
			page.surface_shift_records.end(),
			[&page](const WtChunkSurfaceShiftRecord &record) {
				return record.edge_index < kWtChunkSurfaceEdgeCount &&
					record.unit_offset < page.metadata.cell_spacing &&
					finite(record.sample_a) && finite(record.sample_b) &&
					((record.sample_a.density < page.surface_shift_isovalue) !=
						(record.sample_b.density < page.surface_shift_isovalue));
			}
		);
}

bool pack_page_field_input(
	const WtGpuMeshingShadowRequest &request,
	WtGpuMeshingInputPack &output,
	std::string &error
) {
	if (request.retained_pages.empty() || request.retained_pages.size() > 25) {
		error = "GPU page-field dependency count is outside the bounded contract";
		return false;
	}
	std::vector<const WtGpuMeshingShadowPage *> pages;
	pages.reserve(request.retained_pages.size());
	for (const WtGpuMeshingShadowPage &retained : request.retained_pages) {
		if (!valid_page_field_input(retained, request.job.source_revision)) {
			error = "GPU page-field dependency is invalid";
			return false;
		}
		pages.push_back(&retained);
	}
	std::sort(pages.begin(), pages.end(), [](const auto *left, const auto *right) {
		return left->key < right->key;
	});
	if (std::adjacent_find(
			pages.begin(), pages.end(), [](const auto *left, const auto *right) {
				return left->key == right->key;
			}) != pages.end()) {
		error = "GPU page-field dependencies contain a duplicate page";
		return false;
	}
	const auto primary = std::find_if(
		pages.begin(), pages.end(), [&request](const auto *retained) {
			return retained->key == request.job.key;
		}
	);
	if (primary == pages.end()) {
		error = "GPU page-field input lacks its primary page";
		return false;
	}

	output.page_field_input = true;
	output.cell_count = static_cast<std::size_t>(
		kWtChunkCellsPerAxis * kWtChunkCellsPerAxis * kWtChunkCellsPerAxis +
		face_count(request.cached_transition_mask) *
			kWtChunkCellsPerAxis * kWtChunkCellsPerAxis
	);
	std::size_t surface_shift_record_count = 0;
	for (const WtGpuMeshingShadowPage *retained : pages) {
		surface_shift_record_count += retained->page->surface_shift_records.size();
	}
	output.sample_count = pages.size() * kWtChunkPageSampleCount;
	output.field_values.reserve(
		(output.sample_count + surface_shift_record_count * 2U) * 4U
	);
	output.field_meta.reserve(
		(output.sample_count + surface_shift_record_count * 2U) * 4U
	);
	output.cell_headers.reserve(pages.size() * 4U);
	output.cell_origins.reserve(pages.size() * 4U);
	output.cell_options.reserve(pages.size() * 4U);
	bool has_inside_sample = false;
	bool has_outside_sample = false;
	for (std::size_t page_index = 0; page_index < pages.size(); ++page_index) {
		const WtChunkPage &page = *pages[page_index]->page;
		const WtGridPoint minimum = wt_chunk_bounds(page.metadata.key).minimum;
		const std::size_t sample_offset = page_index * kWtChunkPageSampleCount;
		output.cell_headers.insert(output.cell_headers.end(), {
			0,
			0,
			i32_bits(page.surface_shift_isovalue),
			1,
		});
		output.cell_origins.insert(output.cell_origins.end(), {
			static_cast<float>(minimum.x),
			static_cast<float>(minimum.y),
			static_cast<float>(minimum.z),
			static_cast<float>(page.metadata.cell_spacing),
		});
		output.cell_options.insert(output.cell_options.end(), {
			static_cast<float>(sample_offset),
			static_cast<float>(page.metadata.dimension_x),
			static_cast<float>(page.metadata.sample_minimum),
			static_cast<float>(page.metadata.sample_maximum),
		});
		for (const WtScalarSample &sample : page.samples) {
			if (!finite(sample.density) || !finite(sample.static_water_density)) {
				error = "GPU page-field sample is non-finite";
				return false;
			}
			const float surface_density =
				request.surface == WtGpuMeshingShadowSurface::StaticWater ?
					sample.static_water_density : sample.density;
			has_inside_sample = has_inside_sample || surface_density < 0.0F;
			has_outside_sample = has_outside_sample || surface_density >= 0.0F;
			output.field_values.insert(output.field_values.end(), {
				sample.density, sample.static_water_density, 0.0F, 0.0F,
			});
			output.field_meta.insert(output.field_meta.end(), {
				static_cast<std::int32_t>(sample.material),
				sample.material_authored ? 1 : 0,
				static_cast<std::int32_t>(page_index),
				1,
			});
		}
	}
	output.proven_empty = !(has_inside_sample && has_outside_sample);
	// Binding 5 carries the authoritative baked surface-shift records in
	// page-field mode. The leading sentinel keeps the stable buffer non-empty
	// for LOD0 pages, which correctly have no shift records.
	output.sample_references.push_back(0);
	for (std::size_t page_index = 0; page_index < pages.size(); ++page_index) {
		const WtChunkPage &page = *pages[page_index]->page;
		const std::size_t header_offset = page_index * 4U;
		output.cell_headers[header_offset] = static_cast<std::int32_t>(
			output.sample_references.size()
		);
		output.cell_headers[header_offset + 1U] = static_cast<std::int32_t>(
			page.surface_shift_records.size()
		);
		for (const WtChunkSurfaceShiftRecord &record :
				page.surface_shift_records) {
			const std::int32_t sample_a_index = static_cast<std::int32_t>(
				output.sample_count++
			);
			append_packed_field_sample(
				record.sample_a, static_cast<std::int32_t>(page_index), output
			);
			const std::int32_t sample_b_index = static_cast<std::int32_t>(
				output.sample_count++
			);
			append_packed_field_sample(
				record.sample_b, static_cast<std::int32_t>(page_index), output
			);
			output.sample_references.insert(output.sample_references.end(), {
				static_cast<std::int32_t>(record.edge_index),
				static_cast<std::int32_t>(record.unit_offset),
				sample_a_index,
				sample_b_index,
			});
		}
	}
	const WtChunkBounds chunk_bounds_value = wt_chunk_bounds(request.job.key);
	output.bounds_min = {
		static_cast<float>(chunk_bounds_value.minimum.x),
		static_cast<float>(chunk_bounds_value.minimum.y),
		static_cast<float>(chunk_bounds_value.minimum.z),
	};
	output.bounds_max = {
		static_cast<float>(chunk_bounds_value.maximum.x),
		static_cast<float>(chunk_bounds_value.maximum.y),
		static_cast<float>(chunk_bounds_value.maximum.z),
	};
	const std::uint64_t source_revision = request.job.source_revision;
	const std::uint64_t world_revision = request.job.world_revision;
	output.config = {
		static_cast<std::int32_t>(output.cell_count),
		static_cast<std::int32_t>(output.sample_count),
		static_cast<std::int32_t>(pages.size()),
		1,
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
		static_cast<std::int32_t>(request.cached_transition_mask),
	};
	const WtTransvoxelTablePack &tables = wt_get_transvoxel_mit_table_pack();
	output.packed_byte_count =
		byte_size(output.field_values) + byte_size(output.field_meta) +
		byte_size(output.cell_headers) + byte_size(output.cell_origins) +
		byte_size(output.cell_options) + byte_size(output.sample_references) +
		sizeof(output.config) + sizeof(tables.regular_cell_class) +
		sizeof(tables.regular_cell_data) + sizeof(tables.regular_vertex_data) +
		sizeof(tables.transition_cell_class) +
		sizeof(tables.transition_cell_data) +
		sizeof(tables.transition_vertex_data);
	return true;
}

} // namespace

bool wt_pack_gpu_meshing_input(
	const WtGpuMeshingShadowRequest &request,
	WtGpuMeshingInputPack &output,
	std::string &error
) {
	output = {};
	error.clear();
	if (request.capture_stage == WtGpuMeshingCaptureStage::PreMeshField &&
		request.records.empty()) {
		if (!wt_is_valid_chunk_key(request.job.key)) {
			error = "GPU meshing chunk key is invalid";
			return false;
		}
		return pack_page_field_input(request, output, error);
	}
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
	output.proven_empty = true;

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
		bool cell_has_inside_sample = false;
		bool cell_has_outside_sample = false;
		for (unsigned int index = 0; index < sample_count; ++index) {
			const WtCellSample &sample = transition ?
				record.transition_input.samples[index] :
				record.regular_input.samples[index];
			if (!finite(sample.density) || !finite(sample.gradient)) {
				error = "GPU meshing sample is non-finite";
				return false;
			}
			cell_has_inside_sample =
				cell_has_inside_sample || sample.density < isovalue;
			cell_has_outside_sample =
				cell_has_outside_sample || sample.density >= isovalue;
			append_sample(
				sample,
				static_cast<std::int32_t>(output.sample_count),
				output
			);
			++output.sample_count;
		}
		if (cell_has_inside_sample && cell_has_outside_sample) {
			output.proven_empty = false;
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
