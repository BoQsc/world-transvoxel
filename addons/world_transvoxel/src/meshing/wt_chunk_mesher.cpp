#include "meshing/wt_chunk_mesher.h"

#include "meshing/wt_chunk_mesh_finalize.h"
#include "meshing/wt_chunk_mesh_geometry.h"
#include "meshing/wt_chunk_mesh_key.h"
#include "meshing/wt_multiresolution_vertex_resolver.h"
#include "meshing/wt_transition_face_constraints.h"

#include <algorithm>
#include <cmath>

namespace world_transvoxel {
namespace {

struct WtIntegerVector {
	int x;
	int y;
	int z;
};

struct WtFaceBasis {
	WtIntegerVector u;
	WtIntegerVector v;
	WtIntegerVector w;
	WtTransitionOrientation orientation;
};

WtGridPoint add(const WtGridPoint &point, const WtIntegerVector &vector, std::int64_t scale) noexcept {
	return {
		point.x + static_cast<std::int64_t>(vector.x) * scale,
		point.y + static_cast<std::int64_t>(vector.y) * scale,
		point.z + static_cast<std::int64_t>(vector.z) * scale,
	};
}

WtVec3 add(const WtVec3 &point, const WtIntegerVector &vector, float scale) noexcept {
	return {
		point.x + static_cast<float>(vector.x) * scale,
		point.y + static_cast<float>(vector.y) * scale,
		point.z + static_cast<float>(vector.z) * scale,
	};
}

WtChunkMeshingStatus get_scalar_sample(
	const WtGridPoint &point,
	const WtChunkSampleSource &source,
	WtChunkMeshingScratch &scratch,
	WtScalarSample &output
) {
	const auto found = scratch.scalar_samples.find(point);
	if (found != scratch.scalar_samples.end()) {
		output = found->second;
		return WtChunkMeshingStatus::Ok;
	}
	if (scratch.scalar_samples.size() >= kWtMaximumCachedScalarSamples) {
		return WtChunkMeshingStatus::SampleCacheOverflow;
	}
	if (!source.sample(point, output) || !std::isfinite(output.density)) {
		return WtChunkMeshingStatus::SampleSourceFailure;
	}
	scratch.scalar_samples.emplace(point, output);
	return WtChunkMeshingStatus::Ok;
}

WtChunkMeshingStatus calculate_cell_sample(
	const WtGridPoint &point,
	std::int64_t gradient_step,
	const WtChunkSampleSource &source,
	WtChunkMeshingScratch &scratch,
	WtCellSample &output
) {
	WtScalarSample center;
	WtScalarSample negative_x;
	WtScalarSample positive_x;
	WtScalarSample negative_y;
	WtScalarSample positive_y;
	WtScalarSample negative_z;
	WtScalarSample positive_z;
	const WtGridPoint offsets[6] = {
		{ point.x - gradient_step, point.y, point.z },
		{ point.x + gradient_step, point.y, point.z },
		{ point.x, point.y - gradient_step, point.z },
		{ point.x, point.y + gradient_step, point.z },
		{ point.x, point.y, point.z - gradient_step },
		{ point.x, point.y, point.z + gradient_step },
	};
	WtScalarSample *neighbors[6] = {
		&negative_x, &positive_x, &negative_y, &positive_y, &negative_z, &positive_z
	};
	WtChunkMeshingStatus status = get_scalar_sample(point, source, scratch, center);
	for (unsigned int index = 0; status == WtChunkMeshingStatus::Ok && index < 6; ++index) {
		status = get_scalar_sample(offsets[index], source, scratch, *neighbors[index]);
	}
	if (status != WtChunkMeshingStatus::Ok) {
		return status;
	}

	output = {
		center.density,
		{
			(positive_x.density - negative_x.density) * 0.5F,
			(positive_y.density - negative_y.density) * 0.5F,
			(positive_z.density - negative_z.density) * 0.5F,
		},
		center.material,
	};
	return WtChunkMeshingStatus::Ok;
}

WtChunkMeshingStatus get_cell_sample(
	const WtGridPoint &point,
	std::int64_t gradient_step,
	const WtChunkSampleSource &source,
	WtChunkMeshingScratch &scratch,
	WtCellSample &output
) {
	const auto found = scratch.cell_samples.find(point);
	if (found != scratch.cell_samples.end()) {
		output = found->second;
		return WtChunkMeshingStatus::Ok;
	}
	if (scratch.cell_samples.size() >= kWtMaximumCachedCellSamples) {
		return WtChunkMeshingStatus::SampleCacheOverflow;
	}
	const WtChunkMeshingStatus status = calculate_cell_sample(
		point, gradient_step, source, scratch, output
	);
	if (status != WtChunkMeshingStatus::Ok) {
		return status;
	}
	scratch.cell_samples.emplace(point, output);
	return WtChunkMeshingStatus::Ok;
}

WtFaceBasis get_face_basis(WtChunkFace face) noexcept {
	switch (face) {
		case WtChunkFace::NegativeX:
			return { { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 }, WtTransitionOrientation::PositiveX };
		case WtChunkFace::PositiveX:
			return { { 0, 1, 0 }, { 0, 0, -1 }, { -1, 0, 0 }, WtTransitionOrientation::NegativeX };
		case WtChunkFace::NegativeY:
			return { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, WtTransitionOrientation::PositiveY };
		case WtChunkFace::PositiveY:
			return { { 0, 0, 1 }, { -1, 0, 0 }, { 0, -1, 0 }, WtTransitionOrientation::NegativeY };
		case WtChunkFace::NegativeZ:
			return { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, WtTransitionOrientation::PositiveZ };
		case WtChunkFace::PositiveZ:
			return { { 1, 0, 0 }, { 0, -1, 0 }, { 0, 0, -1 }, WtTransitionOrientation::NegativeZ };
	}
	return {};
}

WtGridPoint face_origin(WtChunkFace face, const WtFaceBasis &basis, std::int64_t extent) noexcept {
	WtGridPoint origin;
	switch (face) {
		case WtChunkFace::NegativeX: origin.x = 0; break;
		case WtChunkFace::PositiveX: origin.x = extent; break;
		case WtChunkFace::NegativeY: origin.y = 0; break;
		case WtChunkFace::PositiveY: origin.y = extent; break;
		case WtChunkFace::NegativeZ: origin.z = 0; break;
		case WtChunkFace::PositiveZ: origin.z = extent; break;
	}
	if (basis.u.x < 0 || basis.v.x < 0) origin.x = extent;
	if (basis.u.y < 0 || basis.v.y < 0) origin.y = extent;
	if (basis.u.z < 0 || basis.v.z < 0) origin.z = extent;
	return origin;
}

WtVec3 to_vec3(const WtGridPoint &point) noexcept {
	return {
		static_cast<float>(point.x),
		static_cast<float>(point.y),
		static_cast<float>(point.z),
	};
}

WtVec3 to_local_vec3(
	const WtGridPoint &point,
	const WtGridPoint &origin
) noexcept {
	return {
		static_cast<float>(point.x - origin.x),
		static_cast<float>(point.y - origin.y),
		static_cast<float>(point.z - origin.z),
	};
}

WtVec3 add(const WtVec3 &a, const WtVec3 &b) noexcept {
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}

WtVec3 subtract(const WtVec3 &a, const WtVec3 &b) noexcept {
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

std::int64_t grid_edge_length(
	const WtGridPoint &a,
	const WtGridPoint &b
) noexcept {
	const std::int64_t differences[3] = {
		b.x - a.x,
		b.y - a.y,
		b.z - a.z,
	};
	std::int64_t length = 0;
	unsigned int different_axes = 0;
	for (const std::int64_t difference : differences) {
		if (difference == 0) {
			continue;
		}
		++different_axes;
		length = difference < 0 ? -difference : difference;
	}
	return different_axes == 1 ? length : 0;
}

WtChunkMeshingStatus surface_shift_status(
	WtMultiresolutionVertexStatus status
) noexcept {
	switch (status) {
		case WtMultiresolutionVertexStatus::Ok:
			return WtChunkMeshingStatus::Ok;
		case WtMultiresolutionVertexStatus::SampleSourceFailure:
			return WtChunkMeshingStatus::SampleSourceFailure;
		case WtMultiresolutionVertexStatus::SampleCacheOverflow:
		case WtMultiresolutionVertexStatus::CellSampleCacheOverflow:
			return WtChunkMeshingStatus::SampleCacheOverflow;
		case WtMultiresolutionVertexStatus::InvalidEdge:
			return WtChunkMeshingStatus::CellBackendFailure;
	}
	return WtChunkMeshingStatus::CellBackendFailure;
}

WtChunkMeshingStatus append_cell_mesh(
	const WtCellMesh &cell,
	WtChunkMeshBuffer &output,
	WtChunkMeshingScratch &scratch,
	const std::array<WtVec3, kWtTransitionTopologySampleCount> &endpoint_positions,
	const std::array<WtGridPoint, kWtTransitionTopologySampleCount> &endpoint_world_positions,
	const WtGridPoint &world_origin,
	const WtChunkSampleSource &source,
	const WtCellSample *samples,
	unsigned int topology_sample_count,
	float isovalue,
	std::uint8_t deformation_mask,
	float cell_size,
	float width,
	float extent,
	int primary_transition_face,
	WtChunkMeshingStatus overflow_status
) {
	if (cell.vertex_count > kWtCellMaxVertexCount ||
		cell.index_count > kWtCellMaxIndexCount ||
		(cell.index_count % 3U) != 0) {
		return WtChunkMeshingStatus::CellBackendFailure;
	}
	if (output.indices.size() + cell.index_count > output.index_limit) {
		return overflow_status;
	}
	std::array<std::uint32_t, kWtCellMaxVertexCount> mapped{};
	std::array<bool, kWtCellMaxVertexCount> mapped_valid{};
	for (std::uint8_t cell_index = 0; cell_index < cell.index_count; ++cell_index) {
		const std::uint8_t source_index = cell.indices[cell_index];
		if (source_index >= cell.vertex_count) {
			return WtChunkMeshingStatus::CellBackendFailure;
		}
		if (!mapped_valid[source_index]) {
			WtCellVertex vertex = cell.vertices[source_index];
			if (vertex.endpoint_a >= topology_sample_count ||
				vertex.endpoint_b >= topology_sample_count ||
				vertex.endpoint_a == vertex.endpoint_b ||
				samples[vertex.endpoint_a].density == samples[vertex.endpoint_b].density) {
				return WtChunkMeshingStatus::CellBackendFailure;
			}
			const WtCellSample *sample_a = &samples[vertex.endpoint_a];
			const WtCellSample *sample_b = &samples[vertex.endpoint_b];
			WtVec3 position_a = endpoint_positions[vertex.endpoint_a];
			WtVec3 position_b = endpoint_positions[vertex.endpoint_b];
			const std::int64_t edge_length = grid_edge_length(
				endpoint_world_positions[vertex.endpoint_a],
				endpoint_world_positions[vertex.endpoint_b]
			);
			WtResolvedMultiresolutionEdge resolved_edge;
			if (edge_length > 1) {
				const WtMultiresolutionVertexStatus resolve_status =
					wt_resolve_multiresolution_edge(
						endpoint_world_positions[vertex.endpoint_a],
						endpoint_world_positions[vertex.endpoint_b],
						isovalue,
						source,
						scratch.multiresolution,
						resolved_edge
					);
				if (resolve_status != WtMultiresolutionVertexStatus::Ok) {
					return surface_shift_status(resolve_status);
				}
				const WtVec3 endpoint_a_offset = subtract(
					position_a,
					to_local_vec3(
						endpoint_world_positions[vertex.endpoint_a], world_origin
					)
				);
				const WtVec3 endpoint_b_offset = subtract(
					position_b,
					to_local_vec3(
						endpoint_world_positions[vertex.endpoint_b], world_origin
					)
				);
				position_a = add(
					to_local_vec3(resolved_edge.endpoint_a, world_origin),
					endpoint_a_offset
				);
				position_b = add(
					to_local_vec3(resolved_edge.endpoint_b, world_origin),
					endpoint_b_offset
				);
				sample_a = &resolved_edge.sample_a;
				sample_b = &resolved_edge.sample_b;
			}
			vertex.position = wt_canonical_edge_position(
				position_a, position_b, *sample_a, *sample_b, isovalue
			);
			vertex.normal = wt_interpolated_mesh_normal(
				*sample_a, *sample_b, isovalue
			);
			vertex.material = wt_closest_isosurface_endpoint_material(
				*sample_a, *sample_b, isovalue
			);
			vertex.position = wt_deform_chunk_position(
				vertex.position,
				vertex.normal,
				deformation_mask,
				cell_size,
				width,
				extent,
				primary_transition_face
			);
			vertex.position = wt_snap_chunk_position(vertex.position);
			vertex.endpoint_a = 0;
			vertex.endpoint_b = 0;
			const WtChunkVertexKey key = wt_make_chunk_vertex_key(vertex);
			const auto found = scratch.vertices.find(key);
			if (found != scratch.vertices.end()) {
				mapped[source_index] = found->second;
			} else {
				if (output.vertices.size() >= output.vertex_limit) {
					return overflow_status;
				}
				const std::uint32_t output_index =
					static_cast<std::uint32_t>(output.vertices.size());
				output.vertices.push_back(vertex);
				scratch.vertices.emplace(key, output_index);
				mapped[source_index] = output_index;
			}
			mapped_valid[source_index] = true;
		}
		output.indices.push_back(mapped[source_index]);
	}
	return WtChunkMeshingStatus::Ok;
}

WtChunkMeshingStatus mesh_regular_cells(
	const WtChunkMeshingInput &input,
	const WtChunkSampleSource &source,
	const WtMeshingBackend &backend,
	WtChunkMeshResult &output,
	WtChunkMeshingScratch &scratch
) {
	const WtChunkBounds bounds = wt_chunk_bounds(input.key);
	const std::int64_t spacing_integer = wt_lod_cell_size(input.key.lod);
	const float spacing = static_cast<float>(spacing_integer);
	const float extent = static_cast<float>(wt_chunk_extent(input.key.lod));
	const float width = spacing * input.transition_width_ratio;
	scratch.reset_vertices();

	for (int z = 0; z < kWtChunkCellsPerAxis; ++z) {
		for (int y = 0; y < kWtChunkCellsPerAxis; ++y) {
			for (int x = 0; x < kWtChunkCellsPerAxis; ++x) {
				WtRegularCellInput cell_input;
				std::array<WtGridPoint, kWtTransitionTopologySampleCount>
					endpoint_world_positions{};
				cell_input.isovalue = input.isovalue;
				cell_input.cell_size = spacing;
				cell_input.origin = {
					static_cast<float>(x) * spacing,
					static_cast<float>(y) * spacing,
					static_cast<float>(z) * spacing,
				};
				for (unsigned int corner = 0; corner < 8; ++corner) {
					const WtGridPoint point = {
						bounds.minimum.x + static_cast<std::int64_t>(x + ((corner & 1U) != 0U)) * spacing_integer,
						bounds.minimum.y + static_cast<std::int64_t>(y + ((corner & 2U) != 0U)) * spacing_integer,
						bounds.minimum.z + static_cast<std::int64_t>(z + ((corner & 4U) != 0U)) * spacing_integer,
					};
					endpoint_world_positions[corner] = point;
					const WtChunkMeshingStatus sample_status =
						get_cell_sample(
							point,
							spacing_integer,
							source,
							scratch,
							cell_input.samples[corner]
						);
					if (sample_status != WtChunkMeshingStatus::Ok) {
						return sample_status;
					}
				}
				WtCellMesh cell_mesh;
				std::array<WtVec3, kWtTransitionTopologySampleCount> endpoint_positions{};
				for (unsigned int corner = 0; corner < 8; ++corner) {
					endpoint_positions[corner] = {
						cell_input.origin.x + ((corner & 1U) != 0U ? spacing : 0.0F),
						cell_input.origin.y + ((corner & 2U) != 0U ? spacing : 0.0F),
						cell_input.origin.z + ((corner & 4U) != 0U ? spacing : 0.0F),
					};
				}
				const WtCellStatus cell_status = backend.mesh_regular_cell(
					cell_input, cell_mesh, scratch.cell
				);
				if (cell_status == WtCellStatus::Empty) {
					continue;
				}
				if (cell_status != WtCellStatus::Ok) {
					return WtChunkMeshingStatus::CellBackendFailure;
				}
				const WtChunkMeshingStatus append_status = append_cell_mesh(
					cell_mesh,
					output.regular,
					scratch,
					endpoint_positions,
					endpoint_world_positions,
					bounds.minimum,
					source,
					cell_input.samples.data(),
					kWtRegularSampleCount,
					input.isovalue,
					input.transition_mask,
					spacing,
					width,
					extent,
					-1,
					WtChunkMeshingStatus::RegularBufferOverflow
				);
				if (append_status != WtChunkMeshingStatus::Ok) {
					return append_status;
				}
			}
		}
	}
	return WtChunkMeshingStatus::Ok;
}

WtChunkMeshingStatus mesh_transition_face(
	WtChunkFace face,
	const WtChunkMeshingInput &input,
	const WtChunkSampleSource &source,
	const WtMeshingBackend &backend,
	WtChunkMeshResult &output,
	WtChunkMeshingScratch &scratch
) {
	const std::int64_t coarse_spacing = wt_lod_cell_size(input.key.lod);
	const std::int64_t fine_spacing = coarse_spacing / 2;
	const std::int64_t extent_integer = wt_chunk_extent(input.key.lod);
	const float extent = static_cast<float>(extent_integer);
	const float width = static_cast<float>(coarse_spacing) * input.transition_width_ratio;
	const WtChunkBounds bounds = wt_chunk_bounds(input.key);
	const WtFaceBasis basis = get_face_basis(face);
	const WtGridPoint local_face_origin = face_origin(face, basis, extent_integer);
	WtChunkMeshBuffer &face_output = output.transitions[static_cast<std::size_t>(face)];
	scratch.reset_vertices();

	for (int v_cell = 0; v_cell < kWtChunkCellsPerAxis; ++v_cell) {
		for (int u_cell = 0; u_cell < kWtChunkCellsPerAxis; ++u_cell) {
			WtGridPoint local_origin = add(
				add(local_face_origin, basis.u, u_cell * coarse_spacing),
				basis.v,
				v_cell * coarse_spacing
			);
			WtTransitionCellInput cell_input;
			std::array<WtVec3, kWtTransitionTopologySampleCount> endpoint_positions{};
			std::array<WtGridPoint, kWtTransitionTopologySampleCount>
				endpoint_world_positions{};
			std::array<WtGridPoint, 9> full_sample_world_positions{};
			cell_input.isovalue = input.isovalue;
			cell_input.full_resolution_origin = to_vec3(local_origin);
			cell_input.sample_spacing = static_cast<float>(fine_spacing);
			cell_input.transition_width = width;
			cell_input.orientation = basis.orientation;
			for (int v_sample = 0; v_sample < 3; ++v_sample) {
				for (int u_sample = 0; u_sample < 3; ++u_sample) {
					WtGridPoint sample_local = add(
						add(local_origin, basis.u, u_sample * fine_spacing),
						basis.v,
						v_sample * fine_spacing
					);
					const WtGridPoint sample_world = {
						bounds.minimum.x + sample_local.x,
						bounds.minimum.y + sample_local.y,
						bounds.minimum.z + sample_local.z,
					};
					const unsigned int sample_index =
						static_cast<unsigned int>(v_sample * 3 + u_sample);
					full_sample_world_positions[sample_index] = sample_world;
					endpoint_world_positions[sample_index] = sample_world;
					endpoint_positions[sample_index] = to_vec3(sample_local);
					const WtChunkMeshingStatus sample_status = get_cell_sample(
						sample_world,
						fine_spacing,
						source,
						scratch,
						cell_input.samples[sample_index]
					);
					if (sample_status != WtChunkMeshingStatus::Ok) {
						return sample_status;
					}
				}
			}
			constexpr std::array<unsigned int, 4> aliases = { 0, 2, 6, 8 };
			std::array<WtCellSample, kWtTransitionTopologySampleCount> endpoint_samples{};
			for (unsigned int sample_index = 0; sample_index < 9; ++sample_index) {
				endpoint_samples[sample_index] = cell_input.samples[sample_index];
			}
			for (unsigned int alias_index = 0; alias_index < aliases.size(); ++alias_index) {
				const unsigned int sample_index = aliases[alias_index];
				endpoint_world_positions[9 + alias_index] =
					full_sample_world_positions[sample_index];
				endpoint_positions[9 + alias_index] = add(
					endpoint_positions[sample_index], basis.w, width
				);
				const WtChunkMeshingStatus sample_status = calculate_cell_sample(
					full_sample_world_positions[sample_index],
					coarse_spacing,
					source,
					scratch,
					endpoint_samples[9 + alias_index]
				);
				if (sample_status != WtChunkMeshingStatus::Ok) {
					return sample_status;
				}
			}
			WtCellMesh cell_mesh;
			const WtCellStatus cell_status = backend.mesh_transition_cell(
				cell_input, cell_mesh, scratch.cell
			);
			if (cell_status == WtCellStatus::Empty) {
				continue;
			}
			if (cell_status != WtCellStatus::Ok) {
				return WtChunkMeshingStatus::CellBackendFailure;
			}
			const WtChunkMeshingStatus append_status = append_cell_mesh(
				cell_mesh,
				face_output,
				scratch,
				endpoint_positions,
				endpoint_world_positions,
				bounds.minimum,
				source,
				endpoint_samples.data(),
				kWtTransitionTopologySampleCount,
				input.isovalue,
				input.transition_mask,
				static_cast<float>(coarse_spacing),
				width,
				extent,
				static_cast<int>(face),
				WtChunkMeshingStatus::TransitionBufferOverflow
			);
			if (append_status != WtChunkMeshingStatus::Ok) {
				return append_status;
			}
		}
	}
	return WtChunkMeshingStatus::Ok;
}

} // namespace

void WtChunkMeshBuffer::prepare(
	std::size_t maximum_vertices,
	std::size_t maximum_indices
) {
	vertex_limit = maximum_vertices;
	index_limit = maximum_indices;
	vertices.clear();
	indices.clear();
	vertices.reserve(maximum_vertices);
	indices.reserve(maximum_indices);
}

void WtChunkMeshBuffer::clear() noexcept {
	vertices.clear();
	indices.clear();
}

void WtChunkMeshResult::clear() noexcept {
	key = {};
	world_origin = {};
	transition_mask = 0;
	regular.clear();
	for (WtChunkMeshBuffer &transition : transitions) {
		transition.clear();
	}
}

WtChunkMeshingScratch::WtChunkMeshingScratch() {
	scalar_samples.reserve(kWtMaximumCachedScalarSamples);
	cell_samples.reserve(kWtMaximumCachedCellSamples);
	vertices.reserve(kWtMaximumRegularChunkVertices);
}

void WtChunkMeshingScratch::reset_samples() {
	scalar_samples.clear();
	cell_samples.clear();
	multiresolution.reset();
}

void WtChunkMeshingScratch::reset_vertices() {
	vertices.clear();
}

WtChunkMesher::WtChunkMesher(const WtMeshingBackend &backend) noexcept : backend_(backend) {
}

WtChunkMeshingStatus WtChunkMesher::mesh(
	const WtChunkMeshingInput &input,
	const WtChunkSampleSource &source,
	WtChunkMeshResult &output,
	WtChunkMeshingScratch &scratch
) const {
	output.clear();
	if (!wt_is_valid_chunk_key(input.key) ||
		(input.transition_mask & 0xC0U) != 0 ||
		!std::isfinite(input.isovalue) ||
		!std::isfinite(input.transition_width_ratio) ||
		input.transition_width_ratio <= 0.0F ||
		input.transition_width_ratio >= 1.0F ||
		(input.transition_mask != 0 && input.key.lod == 0)) {
		return WtChunkMeshingStatus::InvalidInput;
	}

	output.key = input.key;
	output.world_origin = wt_chunk_bounds(input.key).minimum;
	output.transition_mask = input.transition_mask;
	output.regular.prepare(
		kWtMaximumRegularChunkVertices,
		kWtMaximumRegularChunkIndices
	);
	for (WtChunkMeshBuffer &transition : output.transitions) {
		transition.prepare(
			kWtMaximumTransitionFaceVertices,
			kWtMaximumTransitionFaceIndices
		);
	}
	scratch.reset_samples();

	WtChunkMeshingStatus status = mesh_regular_cells(
		input, source, backend_, output, scratch
	);
	scratch.cell_samples.clear();
	for (unsigned int face_index = 0;
		status == WtChunkMeshingStatus::Ok && face_index < 6;
		++face_index) {
		const WtChunkFace face = static_cast<WtChunkFace>(face_index);
		if ((input.transition_mask & wt_face_bit(face)) != 0) {
			status = mesh_transition_face(
				face, input, source, backend_, output, scratch
			);
		}
	}
	if (status == WtChunkMeshingStatus::Ok) {
		wt_finalize_deformed_triangles(output.regular);
		for (unsigned int face_index = 0; face_index < 6; ++face_index) {
			WtChunkMeshBuffer &transition = output.transitions[face_index];
			if (!transition.indices.empty() &&
				!wt_preserve_transition_face_constraints(
					transition,
					static_cast<WtChunkFace>(face_index),
					static_cast<float>(wt_chunk_extent(input.key.lod))
				)) {
				status = WtChunkMeshingStatus::TransitionBufferOverflow;
				break;
			}
			wt_finalize_deformed_triangles(transition);
		}
	}
	if (status != WtChunkMeshingStatus::Ok) {
		output.clear();
	}
	return status;
}

} // namespace world_transvoxel
