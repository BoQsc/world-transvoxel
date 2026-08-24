#include "api/world_transvoxel_terrain.h"

#include "diagnostics/wt_gpu_meshing_godot_codec.h"
#include "diagnostics/wt_gpu_meshing_shadow.h"

#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

namespace world_transvoxel {
namespace {

godot::Dictionary shadow_identity(
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
	identity["field_mode"] = request.surface ==
		WtGpuMeshingShadowSurface::StaticWater ? 1 : 0;
	identity["sample_count"] = sample_count;
	return identity;
}

bool parse_shadow_identity(
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

} // namespace

void WorldTransvoxelTerrain::bind_gpu_meshing_shadow_methods() {
	godot::ClassDB::bind_method(
		godot::D_METHOD("begin_gpu_meshing_shadow", "capacity"),
		&WorldTransvoxelTerrain::begin_gpu_meshing_shadow,
		DEFVAL(3)
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("end_gpu_meshing_shadow"),
		&WorldTransvoxelTerrain::end_gpu_meshing_shadow
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("pop_gpu_meshing_shadow_request"),
		&WorldTransvoxelTerrain::pop_gpu_meshing_shadow_request
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"complete_gpu_meshing_shadow_request",
			"request_id",
			"identity",
			"matched",
			"error"
		),
		&WorldTransvoxelTerrain::complete_gpu_meshing_shadow_request,
		DEFVAL("")
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_gpu_meshing_shadow_metrics"),
		&WorldTransvoxelTerrain::get_gpu_meshing_shadow_metrics
	);
}

bool WorldTransvoxelTerrain::begin_gpu_meshing_shadow(std::int64_t capacity) {
	return gpu_meshing_shadow_ && gpu_meshing_shadow_->begin(
		static_cast<std::size_t>(std::clamp<std::int64_t>(capacity, 0, 16))
	);
}

void WorldTransvoxelTerrain::end_gpu_meshing_shadow() {
	if (gpu_meshing_shadow_) gpu_meshing_shadow_->end();
}

godot::Dictionary WorldTransvoxelTerrain::pop_gpu_meshing_shadow_request() {
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.gpu_meshing_shadow_request.v1";
	result["status"] = "EMPTY";
	result["cpu_render_publication_unchanged"] = true;
	result["cpu_collision_publication_unchanged"] = true;
	result["gpu_publication_enabled"] = false;
	if (!gpu_meshing_shadow_) return result;
	WtGpuMeshingShadowRequest request;
	if (!gpu_meshing_shadow_->pop(request)) return result;
	godot::Dictionary batch = wt_gpu_meshing_shadow_cell_batch(request.records);
	const godot::PackedFloat32Array densities = batch.get(
		"densities", godot::PackedFloat32Array()
	);
	result["status"] = "PASS";
	result["request_id"] = static_cast<std::int64_t>(request.request_id);
	result["identity"] = shadow_identity(request, densities.size());
	result["cell_batch"] = batch;
	return result;
}

godot::Dictionary WorldTransvoxelTerrain::complete_gpu_meshing_shadow_request(
	std::int64_t request_id,
	const godot::Dictionary &identity_dictionary,
	bool matched,
	const godot::String &error
) {
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.gpu_meshing_shadow_completion.v1";
	result["request_id"] = request_id;
	result["status"] = "UNKNOWN_REQUEST";
	result["accepted"] = false;
	result["published"] = false;
	result["cpu_render_publication_unchanged"] = true;
	result["cpu_collision_publication_unchanged"] = true;
	if (!gpu_meshing_shadow_ || request_id <= 0) {
		result["error"] = "GPU shadow request is invalid";
		return result;
	}
	WtGpuMeshingShadowIdentity identity;
	if (!parse_shadow_identity(identity_dictionary, identity)) {
		result["status"] = "IDENTITY_MISMATCH";
		result["error"] = "GPU shadow result identity is invalid";
		return result;
	}
	const WtGpuMeshingShadowCompletion completion =
		gpu_meshing_shadow_->complete(
			static_cast<std::uint64_t>(request_id),
			identity,
			static_cast<std::uint64_t>(std::max<std::int64_t>(
				0, get_world_source_revision()
			)),
			static_cast<std::uint64_t>(std::max<std::int64_t>(
				0, get_world_revision()
			)),
			matched,
			std::string(error.utf8().get_data())
		);
	result["status"] = wt_gpu_meshing_shadow_completion_status_name(
		completion.status
	);
	result["accepted"] = completion.status ==
			WtGpuMeshingShadowCompletionStatus::Matched ||
		completion.status == WtGpuMeshingShadowCompletionStatus::Mismatched;
	result["error"] = completion.error.c_str();
	return result;
}

godot::Dictionary WorldTransvoxelTerrain::get_gpu_meshing_shadow_metrics() const {
	const WtGpuMeshingShadowMetrics metrics = gpu_meshing_shadow_ ?
		gpu_meshing_shadow_->metrics() : WtGpuMeshingShadowMetrics{};
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.gpu_meshing_shadow_metrics.v1";
	result["enabled"] = metrics.enabled;
	result["capacity"] = static_cast<std::int64_t>(metrics.capacity);
	result["queued_requests"] = static_cast<std::int64_t>(metrics.queued_requests);
	result["in_flight_requests"] = static_cast<std::int64_t>(metrics.in_flight_requests);
	result["captured_requests"] = static_cast<std::int64_t>(metrics.captured_requests);
	result["capacity_rejections"] = static_cast<std::int64_t>(metrics.capacity_rejections);
	result["matched_results"] = static_cast<std::int64_t>(metrics.matched_results);
	result["mismatched_results"] = static_cast<std::int64_t>(metrics.mismatched_results);
	result["stale_results"] = static_cast<std::int64_t>(metrics.stale_results);
	result["unknown_results"] = static_cast<std::int64_t>(metrics.unknown_results);
	result["identity_mismatches"] = static_cast<std::int64_t>(metrics.identity_mismatches);
	result["cpu_render_authority"] = true;
	result["cpu_collision_authority"] = true;
	result["gpu_publication_enabled"] = false;
	return result;
}

} // namespace world_transvoxel
