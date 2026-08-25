#include "diagnostics/wt_gpu_meshing_godot_codec.h"

#include "core/wt_chunk_key.h"
#include "diagnostics/wt_gpu_meshing_shadow.h"

#include <cstdint>

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
