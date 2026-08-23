#include "diagnostics/world_transvoxel_cell_probe.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "meshing/wt_chunk_mesher.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace world_transvoxel {
namespace {

template <std::size_t Size>
godot::PackedInt32Array to_packed_int32(
	const std::array<std::uint32_t, Size> &values
) {
	godot::PackedInt32Array result;
	result.resize(static_cast<std::int64_t>(Size));
	for (std::size_t index = 0; index < Size; ++index) {
		result.set(
			static_cast<std::int64_t>(index),
			static_cast<std::int32_t>(values[index])
		);
	}
	return result;
}

constexpr std::array<std::uint8_t, 9> kTransitionCaseBitSamples = {
	0, 1, 2, 5, 8, 7, 6, 3, 4
};

const char *cell_status_name(WtCellStatus status) noexcept {
	switch (status) {
		case WtCellStatus::Ok:
			return "Ok";
		case WtCellStatus::Empty:
			return "Empty";
		case WtCellStatus::NonFiniteInput:
			return "NonFiniteInput";
		case WtCellStatus::InvalidScale:
			return "InvalidScale";
		case WtCellStatus::InvalidOrientation:
			return "InvalidOrientation";
		case WtCellStatus::TopologyFailure:
			return "TopologyFailure";
	}
	return "Unknown";
}

godot::Vector3 to_godot(const WtVec3 &value) {
	return { value.x, value.y, value.z };
}

WtVec3 from_godot(const godot::Vector3 &value) noexcept {
	return {
		static_cast<float>(value.x),
		static_cast<float>(value.y),
		static_cast<float>(value.z),
	};
}

WtCellSample make_sample(
	const godot::PackedFloat32Array &densities,
	const godot::PackedVector3Array &gradients,
	const godot::PackedInt32Array &materials,
	std::int64_t index
) {
	WtCellSample sample;
	sample.density = densities[index];
	if (index < gradients.size()) {
		sample.gradient = from_godot(gradients[index]);
	} else {
		sample.gradient = { 1.0F, 0.0F, 0.0F };
	}
	if (index < materials.size() && materials[index] > 0) {
		sample.material = static_cast<std::uint16_t>(materials[index]);
	} else {
		sample.material = 1;
	}
	sample.material_authored = index < materials.size();
	return sample;
}

std::int64_t regular_case_code(
	const godot::PackedFloat32Array &densities,
	double isovalue
) noexcept {
	std::int64_t result = 0;
	for (std::int64_t index = 0; index < 8; ++index) {
		if (static_cast<double>(densities[index]) < isovalue) {
			result |= 1LL << index;
		}
	}
	return result;
}

std::int64_t transition_case_code(
	const godot::PackedFloat32Array &densities,
	double isovalue
) noexcept {
	std::int64_t result = 0;
	for (std::int64_t bit = 0; bit < 9; ++bit) {
		const std::int64_t sample_index = kTransitionCaseBitSamples[bit];
		if (static_cast<double>(densities[sample_index]) < isovalue) {
			result |= 1LL << bit;
		}
	}
	return result;
}

const char *chunk_status_name(WtChunkMeshingStatus status) noexcept {
	switch (status) {
		case WtChunkMeshingStatus::Ok:
			return "Ok";
		case WtChunkMeshingStatus::InvalidInput:
			return "InvalidInput";
		case WtChunkMeshingStatus::SampleSourceFailure:
			return "SampleSourceFailure";
		case WtChunkMeshingStatus::SampleCacheOverflow:
			return "SampleCacheOverflow";
		case WtChunkMeshingStatus::RegularBufferOverflow:
			return "RegularBufferOverflow";
		case WtChunkMeshingStatus::TransitionBufferOverflow:
			return "TransitionBufferOverflow";
		case WtChunkMeshingStatus::CellBackendFailure:
			return "CellBackendFailure";
	}
	return "Unknown";
}

const char *chunk_face_name(std::size_t face_index) noexcept {
	switch (static_cast<WtChunkFace>(face_index)) {
		case WtChunkFace::NegativeX:
			return "NegativeX";
		case WtChunkFace::PositiveX:
			return "PositiveX";
		case WtChunkFace::NegativeY:
			return "NegativeY";
		case WtChunkFace::PositiveY:
			return "PositiveY";
		case WtChunkFace::NegativeZ:
			return "NegativeZ";
		case WtChunkFace::PositiveZ:
			return "PositiveZ";
	}
	return "Unknown";
}

bool fits_godot_vector3i_component(std::int64_t value) noexcept {
	return value >= std::numeric_limits<std::int32_t>::min() &&
		value <= std::numeric_limits<std::int32_t>::max();
}

bool fits_godot_vector3i(const WtGridPoint &point) noexcept {
	return fits_godot_vector3i_component(point.x) &&
		fits_godot_vector3i_component(point.y) &&
		fits_godot_vector3i_component(point.z);
}

bool is_numeric_variant(const godot::Variant &value) noexcept {
	return value.get_type() == godot::Variant::INT ||
		value.get_type() == godot::Variant::FLOAT;
}

std::uint16_t material_from_variant(const godot::Variant &value) noexcept {
	std::int64_t material = static_cast<std::int64_t>(value);
	if (material < 0) {
		material = 0;
	}
	if (material > std::numeric_limits<std::uint16_t>::max()) {
		material = std::numeric_limits<std::uint16_t>::max();
	}
	return static_cast<std::uint16_t>(material);
}

class CallableChunkSampleSource final : public WtChunkSampleSource {
public:
	explicit CallableChunkSampleSource(const godot::Callable &callable) :
		callable_(callable) {
	}

	bool sample(const WtGridPoint &point, WtScalarSample &output) const noexcept override {
		if (!callable_.is_valid()) {
			fail("sample callable is invalid");
			return false;
		}
		if (!fits_godot_vector3i(point)) {
			fail("sample grid point is outside Godot Vector3i range");
			return false;
		}

		godot::Array arguments;
		arguments.resize(1);
		arguments[0] = godot::Vector3i(
			static_cast<std::int32_t>(point.x),
			static_cast<std::int32_t>(point.y),
			static_cast<std::int32_t>(point.z)
		);
		++sample_count_;
		return parse_sample(callable_.callv(arguments), output);
	}

	bool failed() const noexcept {
		return failed_;
	}

	std::int64_t sample_count() const noexcept {
		return sample_count_;
	}

	godot::String error() const {
		return error_;
	}

private:
	bool parse_sample(const godot::Variant &value, WtScalarSample &output) const noexcept {
		output = {};
		output.material = 1;
		output.static_water_density = kWtNoStaticWaterDensity;
		if (is_numeric_variant(value)) {
			output.density = static_cast<float>(value);
			if (!std::isfinite(output.density)) {
				fail("sample callable returned non-finite density");
				return false;
			}
			return true;
		}
		if (value.get_type() != godot::Variant::DICTIONARY) {
			fail("sample callable must return a density number or Dictionary");
			return false;
		}

		const godot::Dictionary dictionary = value;
		if (!dictionary.has("density")) {
			fail("sample Dictionary is missing density");
			return false;
		}
		const godot::Variant density_value = dictionary.get("density", godot::Variant());
		if (!is_numeric_variant(density_value)) {
			fail("sample Dictionary density must be numeric");
			return false;
		}
		output.density = static_cast<float>(density_value);
		if (!std::isfinite(output.density)) {
			fail("sample Dictionary density is non-finite");
			return false;
		}

		if (dictionary.has("material")) {
			const godot::Variant material_value = dictionary.get("material", 1);
			if (!is_numeric_variant(material_value)) {
				fail("sample Dictionary material must be numeric");
				return false;
			}
			output.material = material_from_variant(material_value);
		}
		output.material_authored = output.material != 0;
		if (dictionary.has("material_authored")) {
			const godot::Variant authored_value = dictionary.get("material_authored", output.material_authored);
			output.material_authored = authored_value.get_type() == godot::Variant::BOOL ?
				static_cast<bool>(authored_value) : output.material_authored;
		}
		if (dictionary.has("static_water_density")) {
			const godot::Variant water_value = dictionary.get("static_water_density", godot::Variant());
			if (!is_numeric_variant(water_value)) {
				fail("sample Dictionary static_water_density must be numeric");
				return false;
			}
			output.static_water_density = static_cast<float>(water_value);
			if (!std::isfinite(output.static_water_density)) {
				fail("sample Dictionary static_water_density is non-finite");
				return false;
			}
		}
		return true;
	}

	void fail(const char *message) const noexcept {
		if (!failed_) {
			failed_ = true;
			error_ = message;
		}
	}

	godot::Callable callable_;
	mutable bool failed_ = false;
	mutable std::int64_t sample_count_ = 0;
	mutable godot::String error_;
};

godot::Dictionary base_result(const char *cell_type) {
	const WtMeshingBackendInfo &info =
		wt_get_transvoxel_mit_backend().get_info();
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.cell_probe.mesh.v1";
	result["cell_type"] = cell_type;
	result["render_authority"] = "NATIVE_TRANSVOXEL_BACKEND_AUTHORITATIVE";
	result["backend_id"] = info.id;
	result["backend_license"] = info.license;
	result["backend_upstream_revision"] = info.upstream_revision;
	result["vertices"] = godot::PackedVector3Array();
	result["normals"] = godot::PackedVector3Array();
	result["indices"] = godot::PackedInt32Array();
	result["backend_indices"] = godot::PackedInt32Array();
	result["materials"] = godot::PackedInt32Array();
	result["material_authored"] = godot::PackedInt32Array();
	result["endpoint_a"] = godot::PackedInt32Array();
	result["endpoint_b"] = godot::PackedInt32Array();
	result["reuse_data"] = godot::PackedInt32Array();
	result["vertex_count"] = 0;
	result["index_count"] = 0;
	result["triangle_count"] = 0;
	result["ok"] = false;
	result["empty"] = false;
	return result;
}

godot::Dictionary empty_chunk_mesh_buffer_result() {
	godot::Dictionary result;
	result["vertices"] = godot::PackedVector3Array();
	result["normals"] = godot::PackedVector3Array();
	result["indices"] = godot::PackedInt32Array();
	result["backend_indices"] = godot::PackedInt32Array();
	result["materials"] = godot::PackedInt32Array();
	result["material_authored"] = godot::PackedInt32Array();
	result["endpoint_a"] = godot::PackedInt32Array();
	result["endpoint_b"] = godot::PackedInt32Array();
	result["reuse_data"] = godot::PackedInt32Array();
	result["vertex_count"] = 0;
	result["index_count"] = 0;
	result["triangle_count"] = 0;
	return result;
}

godot::Array empty_chunk_transition_results() {
	godot::Array transitions;
	transitions.resize(6);
	for (std::size_t face_index = 0; face_index < 6; ++face_index) {
		godot::Dictionary transition = empty_chunk_mesh_buffer_result();
		transition["face"] = chunk_face_name(face_index);
		transition["face_index"] = static_cast<std::int64_t>(face_index);
		transitions[static_cast<std::int64_t>(face_index)] = transition;
	}
	return transitions;
}

godot::Dictionary chunk_base_result() {
	const WtMeshingBackendInfo &info =
		wt_get_transvoxel_mit_backend().get_info();
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.cell_probe.chunk_mesh.v1";
	result["cell_type"] = "chunk";
	result["render_authority"] = "NATIVE_TRANSVOXEL_BACKEND_AUTHORITATIVE";
	result["backend_id"] = info.id;
	result["backend_license"] = info.license;
	result["backend_upstream_revision"] = info.upstream_revision;
	result["regular"] = empty_chunk_mesh_buffer_result();
	result["transitions"] = empty_chunk_transition_results();
	result["vertex_count"] = 0;
	result["index_count"] = 0;
	result["triangle_count"] = 0;
	result["transition_vertex_count"] = 0;
	result["transition_index_count"] = 0;
	result["transition_triangle_count"] = 0;
	result["ok"] = false;
	result["sample_source_failed"] = false;
	result["sample_count"] = 0;
	return result;
}

void fill_mesh_result(
	godot::Dictionary &result,
	WtCellStatus status,
	const WtCellMesh &mesh
) {
	result["status"] = cell_status_name(status);
	result["status_code"] = static_cast<std::int64_t>(status);
	result["ok"] = status == WtCellStatus::Ok;
	result["empty"] = status == WtCellStatus::Empty;
	result["vertex_count"] = static_cast<std::int64_t>(mesh.vertex_count);
	result["index_count"] = static_cast<std::int64_t>(mesh.index_count);
	result["triangle_count"] = static_cast<std::int64_t>(mesh.index_count / 3U);
	if (status != WtCellStatus::Ok) {
		return;
	}

	godot::PackedVector3Array vertices;
	godot::PackedVector3Array normals;
	godot::PackedInt32Array indices;
	godot::PackedInt32Array backend_indices;
	godot::PackedInt32Array materials;
	godot::PackedInt32Array material_authored;
	godot::PackedInt32Array endpoint_a;
	godot::PackedInt32Array endpoint_b;
	godot::PackedInt32Array reuse_data;
	vertices.resize(mesh.vertex_count);
	normals.resize(mesh.vertex_count);
	materials.resize(mesh.vertex_count);
	material_authored.resize(mesh.vertex_count);
	endpoint_a.resize(mesh.vertex_count);
	endpoint_b.resize(mesh.vertex_count);
	reuse_data.resize(mesh.vertex_count);
	for (std::int64_t index = 0; index < mesh.vertex_count; ++index) {
		const WtCellVertex &vertex = mesh.vertices[index];
		vertices.set(index, to_godot(vertex.position));
		normals.set(index, to_godot(vertex.normal));
		materials.set(index, static_cast<std::int32_t>(vertex.material));
		material_authored.set(index, vertex.material_authored ? 1 : 0);
		endpoint_a.set(index, vertex.endpoint_a);
		endpoint_b.set(index, vertex.endpoint_b);
		reuse_data.set(index, vertex.reuse_data);
	}
	indices.resize(mesh.index_count);
	backend_indices.resize(mesh.index_count);
	for (std::int64_t index = 0; index < mesh.index_count; ++index) {
		backend_indices.set(index, mesh.indices[index]);
	}
	for (std::int64_t triangle = 0; triangle < mesh.index_count; triangle += 3) {
		indices.set(triangle, mesh.indices[triangle]);
		indices.set(triangle + 1, mesh.indices[triangle + 2]);
		indices.set(triangle + 2, mesh.indices[triangle + 1]);
	}
	result["vertices"] = vertices;
	result["normals"] = normals;
	result["indices"] = indices;
	result["backend_indices"] = backend_indices;
	result["materials"] = materials;
	result["material_authored"] = material_authored;
	result["endpoint_a"] = endpoint_a;
	result["endpoint_b"] = endpoint_b;
	result["reuse_data"] = reuse_data;
}

godot::Dictionary chunk_mesh_buffer_result(const WtChunkMeshBuffer &buffer) {
	godot::Dictionary result = empty_chunk_mesh_buffer_result();
	const std::int64_t vertex_count =
		static_cast<std::int64_t>(buffer.vertices.size());
	const std::int64_t index_count =
		static_cast<std::int64_t>(buffer.indices.size());
	result["vertex_count"] = vertex_count;
	result["index_count"] = index_count;
	result["triangle_count"] = index_count / 3;
	if (vertex_count == 0) {
		return result;
	}

	godot::PackedVector3Array vertices;
	godot::PackedVector3Array normals;
	godot::PackedInt32Array indices;
	godot::PackedInt32Array backend_indices;
	godot::PackedInt32Array materials;
	godot::PackedInt32Array material_authored;
	godot::PackedInt32Array endpoint_a;
	godot::PackedInt32Array endpoint_b;
	godot::PackedInt32Array reuse_data;
	vertices.resize(vertex_count);
	normals.resize(vertex_count);
	materials.resize(vertex_count);
	material_authored.resize(vertex_count);
	endpoint_a.resize(vertex_count);
	endpoint_b.resize(vertex_count);
	reuse_data.resize(vertex_count);
	for (std::int64_t index = 0; index < vertex_count; ++index) {
		const WtCellVertex &vertex =
			buffer.vertices[static_cast<std::size_t>(index)];
		vertices.set(index, to_godot(vertex.position));
		normals.set(index, to_godot(vertex.normal));
		materials.set(index, static_cast<std::int32_t>(vertex.material));
		material_authored.set(index, vertex.material_authored ? 1 : 0);
		endpoint_a.set(index, vertex.endpoint_a);
		endpoint_b.set(index, vertex.endpoint_b);
		reuse_data.set(index, vertex.reuse_data);
	}
	indices.resize(index_count);
	backend_indices.resize(index_count);
	for (std::int64_t index = 0; index < index_count; ++index) {
		backend_indices.set(
			index,
			static_cast<std::int32_t>(
				buffer.indices[static_cast<std::size_t>(index)]
			)
		);
	}
	for (std::int64_t triangle = 0; triangle < index_count; triangle += 3) {
		indices.set(
			triangle,
			static_cast<std::int32_t>(
				buffer.indices[static_cast<std::size_t>(triangle)]
			)
		);
		indices.set(
			triangle + 1,
			static_cast<std::int32_t>(
				buffer.indices[static_cast<std::size_t>(triangle + 2)]
			)
		);
		indices.set(
			triangle + 2,
			static_cast<std::int32_t>(
				buffer.indices[static_cast<std::size_t>(triangle + 1)]
			)
		);
	}
	result["vertices"] = vertices;
	result["normals"] = normals;
	result["indices"] = indices;
	result["backend_indices"] = backend_indices;
	result["materials"] = materials;
	result["material_authored"] = material_authored;
	result["endpoint_a"] = endpoint_a;
	result["endpoint_b"] = endpoint_b;
	result["reuse_data"] = reuse_data;
	return result;
}

} // namespace

void WorldTransvoxelCellProbe::_bind_methods() {
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_backend_identity"),
		&WorldTransvoxelCellProbe::get_backend_identity
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_gpu_meshing_tables"),
		&WorldTransvoxelCellProbe::get_gpu_meshing_tables
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"mesh_regular_cell",
			"densities",
			"gradients",
			"materials",
			"origin",
			"cell_size",
			"isovalue"
		),
		&WorldTransvoxelCellProbe::mesh_regular_cell
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"mesh_transition_cell",
			"densities",
			"gradients",
			"materials",
			"orientation",
			"full_resolution_origin",
			"sample_spacing",
			"transition_width",
			"isovalue"
		),
		&WorldTransvoxelCellProbe::mesh_transition_cell
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"mesh_chunk_with_callable",
			"sample_callable",
			"chunk_coordinate",
			"lod",
			"transition_mask",
			"cached_transition_mask",
			"isovalue",
			"transition_width_ratio"
		),
		&WorldTransvoxelCellProbe::mesh_chunk_with_callable
	);
}

godot::Dictionary WorldTransvoxelCellProbe::get_backend_identity() const {
	const WtMeshingBackendInfo &info =
		wt_get_transvoxel_mit_backend().get_info();
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.cell_probe.identity.v1";
	result["available"] = wt_get_transvoxel_mit_backend().is_available();
	result["backend_id"] = info.id;
	result["backend_license"] = info.license;
	result["backend_upstream_revision"] = info.upstream_revision;
	result["regular_case_count"] =
		static_cast<std::int64_t>(info.regular_case_count);
	result["transition_case_count"] =
		static_cast<std::int64_t>(info.transition_case_count);
	result["render_authority"] = "NATIVE_TRANSVOXEL_BACKEND_AUTHORITATIVE";
	return result;
}

godot::Dictionary WorldTransvoxelCellProbe::get_gpu_meshing_tables() const {
	const WtMeshingBackendInfo &info =
		wt_get_transvoxel_mit_backend().get_info();
	const WtTransvoxelTablePack &pack = wt_get_transvoxel_mit_table_pack();
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.cell_probe.gpu_meshing_tables.v1";
	result["backend_id"] = info.id;
	result["backend_upstream_revision"] = info.upstream_revision;
	result["authority"] = "NATIVE_TRANSVOXEL_BACKEND_TABLE_EXPORT";
	result["regular_cell_class"] = to_packed_int32(pack.regular_cell_class);
	result["regular_cell_data"] = to_packed_int32(pack.regular_cell_data);
	result["regular_vertex_data"] = to_packed_int32(pack.regular_vertex_data);
	result["transition_cell_class"] = to_packed_int32(pack.transition_cell_class);
	result["transition_cell_data"] = to_packed_int32(pack.transition_cell_data);
	result["transition_vertex_data"] = to_packed_int32(pack.transition_vertex_data);
	return result;
}

godot::Dictionary WorldTransvoxelCellProbe::mesh_regular_cell(
	const godot::PackedFloat32Array &densities,
	const godot::PackedVector3Array &gradients,
	const godot::PackedInt32Array &materials,
	const godot::Vector3 &origin,
	double cell_size,
	double isovalue
) const {
	godot::Dictionary result = base_result("regular");
	if (densities.size() < 8) {
		result["status"] = "InvalidInput";
		result["error"] = "regular cell requires at least 8 densities";
		return result;
	}
	WtRegularCellInput input;
	input.origin = from_godot(origin);
	input.cell_size = static_cast<float>(cell_size);
	input.isovalue = static_cast<float>(isovalue);
	for (std::int64_t index = 0; index < 8; ++index) {
		input.samples[index] = make_sample(
			densities, gradients, materials, index
		);
	}
	result["case_code"] = regular_case_code(densities, isovalue);
	WtCellMesh mesh;
	WtCellMeshingScratch scratch;
	const WtCellStatus status = wt_get_transvoxel_mit_backend()
		.mesh_regular_cell(input, mesh, scratch);
	fill_mesh_result(result, status, mesh);
	return result;
}

godot::Dictionary WorldTransvoxelCellProbe::mesh_transition_cell(
	const godot::PackedFloat32Array &densities,
	const godot::PackedVector3Array &gradients,
	const godot::PackedInt32Array &materials,
	std::int64_t orientation,
	const godot::Vector3 &full_resolution_origin,
	double sample_spacing,
	double transition_width,
	double isovalue
) const {
	godot::Dictionary result = base_result("transition");
	if (densities.size() < 9) {
		result["status"] = "InvalidInput";
		result["error"] = "transition cell requires at least 9 densities";
		return result;
	}
	WtTransitionCellInput input;
	input.full_resolution_origin = from_godot(full_resolution_origin);
	input.sample_spacing = static_cast<float>(sample_spacing);
	input.transition_width = static_cast<float>(transition_width);
	input.isovalue = static_cast<float>(isovalue);
	input.orientation = static_cast<WtTransitionOrientation>(orientation);
	for (std::int64_t index = 0; index < 9; ++index) {
		input.samples[index] = make_sample(
			densities, gradients, materials, index
		);
	}
	result["case_code"] = transition_case_code(densities, isovalue);
	result["orientation"] = orientation;
	WtCellMesh mesh;
	WtCellMeshingScratch scratch;
	const WtCellStatus status = wt_get_transvoxel_mit_backend()
		.mesh_transition_cell(input, mesh, scratch);
	fill_mesh_result(result, status, mesh);
	return result;
}

godot::Dictionary WorldTransvoxelCellProbe::mesh_chunk_with_callable(
	const godot::Callable &sample_callable,
	const godot::Vector3i &chunk_coordinate,
	std::int64_t lod,
	std::int64_t transition_mask,
	std::int64_t cached_transition_mask,
	double isovalue,
	double transition_width_ratio
) const {
	godot::Dictionary result = chunk_base_result();
	result["chunk_coordinate"] = chunk_coordinate;
	result["lod"] = lod;
	result["transition_mask"] = transition_mask;
	result["cached_transition_mask"] = cached_transition_mask;
	result["isovalue"] = isovalue;
	result["transition_width_ratio"] = transition_width_ratio;
	if (!sample_callable.is_valid() ||
		lod < 0 ||
		lod > kWtMaximumLod ||
		transition_mask < 0 ||
		transition_mask > 0x3F ||
		cached_transition_mask < 0 ||
		cached_transition_mask > 0x3F) {
		result["status"] = chunk_status_name(WtChunkMeshingStatus::InvalidInput);
		result["status_code"] =
			static_cast<std::int64_t>(WtChunkMeshingStatus::InvalidInput);
		result["error"] = "chunk probe input is invalid";
		return result;
	}

	WtChunkMeshingInput input;
	input.key = {
		chunk_coordinate.x,
		chunk_coordinate.y,
		chunk_coordinate.z,
		static_cast<std::uint8_t>(lod),
	};
	input.transition_mask = static_cast<std::uint8_t>(transition_mask);
	input.cached_transition_mask =
		static_cast<std::uint8_t>(cached_transition_mask);
	input.isovalue = static_cast<float>(isovalue);
	input.transition_width_ratio = static_cast<float>(transition_width_ratio);

	CallableChunkSampleSource source(sample_callable);
	WtChunkMeshResult output;
	WtChunkMeshingScratch scratch;
	const WtChunkMeshingStatus status = WtChunkMesher(
		wt_get_transvoxel_mit_backend()
	).mesh(input, source, output, scratch);

	result["status"] = chunk_status_name(status);
	result["status_code"] = static_cast<std::int64_t>(status);
	result["ok"] = status == WtChunkMeshingStatus::Ok;
	result["sample_source_failed"] = source.failed();
	result["sample_count"] = source.sample_count();
	result["sample_error"] = source.error();
	if (status != WtChunkMeshingStatus::Ok) {
		return result;
	}

	result["world_origin_x"] = output.world_origin.x;
	result["world_origin_y"] = output.world_origin.y;
	result["world_origin_z"] = output.world_origin.z;
	result["cached_transition_mask"] =
		static_cast<std::int64_t>(output.cached_transition_mask);
	result["transition_width_ratio"] = output.transition_width_ratio;

	godot::Dictionary regular = chunk_mesh_buffer_result(output.regular);
	godot::Array transitions;
	transitions.resize(6);
	std::int64_t transition_vertex_count = 0;
	std::int64_t transition_index_count = 0;
	std::int64_t transition_triangle_count = 0;
	for (std::size_t face_index = 0; face_index < 6; ++face_index) {
		godot::Dictionary transition =
			chunk_mesh_buffer_result(output.transitions[face_index]);
		transition["face"] = chunk_face_name(face_index);
		transition["face_index"] = static_cast<std::int64_t>(face_index);
		transition_vertex_count +=
			static_cast<std::int64_t>(output.transitions[face_index].vertices.size());
		transition_index_count +=
			static_cast<std::int64_t>(output.transitions[face_index].indices.size());
		transitions[static_cast<std::int64_t>(face_index)] = transition;
	}
	transition_triangle_count = transition_index_count / 3;

	result["regular"] = regular;
	result["transitions"] = transitions;
	result["vertex_count"] =
		static_cast<std::int64_t>(output.regular.vertices.size()) +
		transition_vertex_count;
	result["index_count"] =
		static_cast<std::int64_t>(output.regular.indices.size()) +
		transition_index_count;
	result["triangle_count"] =
		static_cast<std::int64_t>(output.regular.indices.size() / 3U) +
		transition_triangle_count;
	result["transition_vertex_count"] = transition_vertex_count;
	result["transition_index_count"] = transition_index_count;
	result["transition_triangle_count"] = transition_triangle_count;
	return result;
}

} // namespace world_transvoxel
