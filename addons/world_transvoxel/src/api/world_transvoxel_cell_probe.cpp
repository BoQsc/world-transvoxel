#include "api/world_transvoxel_cell_probe.h"

#include "backend/wt_transvoxel_mit_backend.h"

#include <godot_cpp/core/class_db.hpp>

#include <array>
#include <cstdint>

namespace world_transvoxel {
namespace {

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

} // namespace

void WorldTransvoxelCellProbe::_bind_methods() {
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_backend_identity"),
		&WorldTransvoxelCellProbe::get_backend_identity
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

} // namespace world_transvoxel
