#include "diagnostics/wt_gpu_meshing_godot_codec.h"

#include "core/wt_chunk_key.h"
#include "backend/wt_transvoxel_mit_backend.h"
#include "diagnostics/wt_gpu_meshing_input_pack.h"
#include "diagnostics/wt_gpu_meshing_shadow.h"

#include <cstdint>
#include <cstring>

namespace {

template <typename Container>
godot::PackedByteArray to_bytes(const Container &values) {
	godot::PackedByteArray result;
	const std::size_t size = sizeof(typename Container::value_type) * values.size();
	result.resize(static_cast<std::int64_t>(size));
	if (size > 0) std::memcpy(result.ptrw(), values.data(), size);
	return result;
}

} // namespace

namespace world_transvoxel {

godot::Dictionary wt_gpu_meshing_shadow_identity(
	const WtGpuMeshingShadowRequest &request,
	std::int64_t sample_count
) {
	godot::Dictionary identity;
	identity["page_x"] = request.job.key.x;
	identity["page_y"] = request.job.key.y;
	identity["page_z"] = request.job.key.z;
	identity["lod"] = request.job.key.lod;
	identity["generation"] = static_cast<std::int64_t>(
		request.job.generation.value
	);
	identity["source_revision"] = static_cast<std::int64_t>(
		request.job.source_revision
	);
	identity["world_revision"] = static_cast<std::int64_t>(
		request.job.world_revision
	);
	identity["transition_mask"] = request.transition_mask;
	identity["cached_transition_mask"] = request.cached_transition_mask;
	identity["surface"] = wt_gpu_meshing_shadow_surface_name(request.surface);
	identity["static_water_surface_expected"] =
		request.static_water_surface_expected;
	identity["field_mode"] = request.surface ==
		WtGpuMeshingShadowSurface::StaticWater ? 1 : 0;
	identity["sample_count"] = sample_count;
	return identity;
}

godot::Dictionary wt_gpu_meshing_shadow_packed_input(
	const WtGpuMeshingShadowRequest &request
) {
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.gpu_meshing_input_buffers.v1";
	result["status"] = "FAIL";
	result["fallback_used"] = false;
	WtGpuMeshingInputPack packed;
	std::string error;
	if (!wt_pack_gpu_meshing_input(request, packed, error)) {
		result["error"] = error.c_str();
		return result;
	}
	const WtTransvoxelTablePack &tables = wt_get_transvoxel_mit_table_pack();
	godot::Array buffers;
	buffers.resize(13);
	buffers[0] = to_bytes(packed.field_values);
	buffers[1] = to_bytes(packed.field_meta);
	buffers[2] = to_bytes(packed.cell_headers);
	buffers[3] = to_bytes(packed.cell_origins);
	buffers[4] = to_bytes(packed.cell_options);
	buffers[5] = to_bytes(packed.sample_references);
	buffers[6] = to_bytes(packed.config);
	buffers[7] = to_bytes(tables.regular_cell_class);
	buffers[8] = to_bytes(tables.regular_cell_data);
	buffers[9] = to_bytes(tables.regular_vertex_data);
	buffers[10] = to_bytes(tables.transition_cell_class);
	buffers[11] = to_bytes(tables.transition_cell_data);
	buffers[12] = to_bytes(tables.transition_vertex_data);
	result["status"] = "PASS";
	result["error"] = "";
	result["input_buffers"] = buffers;
	result["cell_count"] = static_cast<std::int64_t>(packed.cell_count);
	result["sample_count"] = static_cast<std::int64_t>(packed.sample_count);
	result["bounds_min"] = godot::Vector3(
		packed.bounds_min.x, packed.bounds_min.y, packed.bounds_min.z
	);
	result["bounds_max"] = godot::Vector3(
		packed.bounds_max.x, packed.bounds_max.y, packed.bounds_max.z
	);
	result["packed_byte_count"] = static_cast<std::int64_t>(
		packed.packed_byte_count
	);
	return result;
}

bool wt_parse_gpu_meshing_shadow_identity(
	const godot::Dictionary &dictionary,
	WtGpuMeshingShadowIdentity &identity
) {
	const godot::String surface = dictionary.get("surface", "");
	if (surface != "terrain" && surface != "static_water") return false;
	const std::int64_t generation = dictionary.get("generation", 0);
	const std::int64_t source_revision = dictionary.get("source_revision", 0);
	const std::int64_t world_revision = dictionary.get("world_revision", -1);
	const std::int64_t lod = dictionary.get("lod", -1);
	const std::int64_t transition_mask = dictionary.get("transition_mask", -1);
	if (generation <= 0 || source_revision <= 0 || world_revision < 0 ||
		lod < 0 || lod > kWtMaximumLod || transition_mask < 0 ||
		transition_mask > 0x3f) {
		return false;
	}
	identity.key = {
		static_cast<std::int32_t>(dictionary.get("page_x", 0)),
		static_cast<std::int32_t>(dictionary.get("page_y", 0)),
		static_cast<std::int32_t>(dictionary.get("page_z", 0)),
		static_cast<std::uint8_t>(lod),
	};
	identity.generation.value = static_cast<std::uint64_t>(generation);
	identity.source_revision = static_cast<std::uint64_t>(source_revision);
	identity.world_revision = static_cast<std::uint64_t>(world_revision);
	identity.transition_mask = static_cast<std::uint8_t>(transition_mask);
	identity.surface = surface == "static_water" ?
		WtGpuMeshingShadowSurface::StaticWater :
		WtGpuMeshingShadowSurface::Terrain;
	return wt_is_valid_chunk_key(identity.key);
}

} // namespace world_transvoxel
