#include "api/world_transvoxel_terrain.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "diagnostics/wt_gpu_meshing_godot_codec.h"
#include "diagnostics/wt_gpu_meshing_shadow.h"
#include "meshing/wt_material_volume_sample_source.h"
#include "render/wt_godot_render_sink.h"
#include "render/wt_render_payload.h"
#include "services/wt_chunk_application.h"
#include "services/wt_chunk_resource_payload.h"
#include "storage/wt_chunk_page_sample_source.h"

#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace world_transvoxel {
namespace {

bool build_gpu_cell_render_candidate(
	const WtGpuMeshingShadowRequest &request,
	const godot::Array &gpu_cells,
	std::shared_ptr<WtRenderPayload> &payload,
	godot::String &error
) {
	if (!request.authority_terrain_mesh || !request.authority_water_mesh ||
		request.retained_pages.empty()) {
		error = "GPU publication request lacks retained native authority";
		return false;
	}
	const auto primary = std::find_if(
		request.retained_pages.begin(), request.retained_pages.end(),
		[&request](const WtGpuMeshingShadowPage &page) {
			return page.key == request.job.key && static_cast<bool>(page.page);
		}
	);
	if (primary == request.retained_pages.end()) {
		error = "GPU publication request lacks its primary source page";
		return false;
	}
	WtChunkPageSampleSource source(*primary->page);
	for (const WtGpuMeshingShadowPage &page : request.retained_pages) {
		if (&page == &*primary) continue;
		if (!page.page || source.add_transition_support_page(*page.page) !=
				WtChunkPageSampleSourceStatus::Ok) {
			error = "GPU publication retained source pages are inconsistent";
			return false;
		}
	}
	if (!source.has_transition_support(request.cached_transition_mask)) {
		error = "GPU publication lacks exact transition support pages";
		return false;
	}
	std::vector<WtReplayMeshingCell> replay_cells;
	if (!wt_parse_gpu_replay_cells(gpu_cells, replay_cells, error)) {
		return false;
	}
	WtReplayMeshingBackend replay(
		wt_get_transvoxel_mit_backend(), replay_cells
	);
	WtChunkMeshResult gpu_mesh;
	WtChunkMeshingScratch scratch;
	const WtChunkMeshingInput input{
		request.job.key,
		request.transition_mask,
		request.cached_transition_mask,
		0.0F,
		0.25F,
	};
	WtChunkMeshingStatus status = WtChunkMeshingStatus::CellBackendFailure;
	if (request.surface == WtGpuMeshingShadowSurface::StaticWater) {
		const WtMaterialVolumeSampleSource water_source(
			source, kWtStaticWaterMaterialId
		);
		status = WtChunkMesher(replay).mesh(
			input, water_source, gpu_mesh, scratch
		);
	} else {
		status = WtChunkMesher(replay).mesh(input, source, gpu_mesh, scratch);
	}
	if (status != WtChunkMeshingStatus::Ok || !replay.complete()) {
		error = godot::String("Native GPU-cell finalization failed: ") +
			wt_replay_meshing_failure_name(replay.failure());
		return false;
	}
	const WtChunkMeshResult &authority_surface =
		request.surface == WtGpuMeshingShadowSurface::StaticWater ?
			*request.authority_water_mesh : *request.authority_terrain_mesh;
	if (!wt_equal_mesh_payload(gpu_mesh, authority_surface)) {
		error = "Native GPU-cell finalization differs from CPU authority";
		return false;
	}
	payload = std::make_shared<WtRenderPayload>();
	const WtRenderBuildStatus build_status = request.surface ==
			WtGpuMeshingShadowSurface::StaticWater ?
		wt_build_render_payload(
			*request.authority_terrain_mesh,
			gpu_mesh,
			request.job.generation,
			request.transition_mask,
			*payload
		) :
		wt_build_render_payload(
			gpu_mesh,
			*request.authority_water_mesh,
			request.job.generation,
			request.transition_mask,
			*payload
		);
	WtRenderPayload authority_render;
	const WtRenderBuildStatus authority_status = wt_build_render_payload(
		*request.authority_terrain_mesh,
		*request.authority_water_mesh,
		request.job.generation,
		request.transition_mask,
		authority_render
	);
	if (build_status != WtRenderBuildStatus::Ok ||
		authority_status != WtRenderBuildStatus::Ok ||
		!wt_equal_render_payload(*payload, authority_render)) {
		payload.reset();
		error = "GPU-cell render payload differs from CPU authority";
		return false;
	}
	payload->publication_source = WtRenderPublicationSource::GpuCellCandidate;
	return true;
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
		godot::D_METHOD("begin_gpu_meshing_publication", "capacity"),
		&WorldTransvoxelTerrain::begin_gpu_meshing_publication,
		DEFVAL(3)
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
		godot::D_METHOD(
			"complete_gpu_meshing_publication_request",
			"request_id",
			"identity",
			"gpu_cells",
			"matched",
			"error"
		),
		&WorldTransvoxelTerrain::complete_gpu_meshing_publication_request,
		DEFVAL("")
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_gpu_meshing_shadow_metrics"),
		&WorldTransvoxelTerrain::get_gpu_meshing_shadow_metrics
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("begin_gpu_resident_render_publication", "capacity"),
		&WorldTransvoxelTerrain::begin_gpu_resident_render_publication,
		DEFVAL(3)
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("end_gpu_resident_render_publication"),
		&WorldTransvoxelTerrain::end_gpu_resident_render_publication
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("pop_gpu_resident_render_request"),
		&WorldTransvoxelTerrain::pop_gpu_resident_render_request
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"validate_gpu_resident_render_request", "request_id", "identity"
		),
		&WorldTransvoxelTerrain::validate_gpu_resident_render_request
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"get_gpu_resident_render_chunk_readiness", "identity"
		),
		&WorldTransvoxelTerrain::get_gpu_resident_render_chunk_readiness
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"reject_gpu_resident_render_request",
			"request_id",
			"identity",
			"error"
		),
		&WorldTransvoxelTerrain::reject_gpu_resident_render_request
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"set_gpu_resident_render_chunk_active", "identities", "active"
		),
		&WorldTransvoxelTerrain::set_gpu_resident_render_chunk_active
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"reconcile_gpu_resident_render_chunks", "terrain_identities"
		),
		&WorldTransvoxelTerrain::reconcile_gpu_resident_render_chunks
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_gpu_resident_render_metrics"),
		&WorldTransvoxelTerrain::get_gpu_resident_render_metrics
	);
}

bool WorldTransvoxelTerrain::begin_gpu_meshing_shadow(std::int64_t capacity) {
	if (gpu_resident_render_publication_enabled_) {
		end_gpu_resident_render_publication();
	}
	gpu_meshing_publication_enabled_ = false;
	return gpu_meshing_shadow_ && gpu_meshing_shadow_->begin(
		static_cast<std::size_t>(std::clamp<std::int64_t>(capacity, 0, 16))
	);
}

bool WorldTransvoxelTerrain::begin_gpu_meshing_publication(
	std::int64_t capacity
) {
	if (gpu_resident_render_publication_enabled_) {
		end_gpu_resident_render_publication();
	}
	gpu_meshing_publication_enabled_ = false;
	gpu_meshing_publication_attempts_ = 0;
	gpu_meshing_publication_queued_ = 0;
	gpu_meshing_publication_finalization_rejections_ = 0;
	gpu_meshing_publication_application_rejections_ = 0;
	gpu_meshing_publication_stale_application_skips_ = 0;
	gpu_meshing_publication_terrain_queued_ = 0;
	gpu_meshing_publication_water_queued_ = 0;
	if (!gpu_meshing_shadow_ || !gpu_meshing_shadow_->begin(
			static_cast<std::size_t>(std::clamp<std::int64_t>(capacity, 0, 16)),
			true
		)) {
		return false;
	}
	gpu_meshing_publication_enabled_ = true;
	return true;
}

void WorldTransvoxelTerrain::end_gpu_meshing_shadow() {
	gpu_meshing_publication_enabled_ = false;
	if (gpu_meshing_shadow_) gpu_meshing_shadow_->end();
}

godot::Dictionary WorldTransvoxelTerrain::pop_gpu_meshing_shadow_request() {
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.gpu_meshing_shadow_request.v1";
	result["status"] = "EMPTY";
	result["cpu_render_publication_unchanged"] = true;
	result["cpu_collision_publication_unchanged"] = true;
	result["gpu_publication_enabled"] = gpu_meshing_publication_enabled_;
	result["gpu_resident_render_publication"] = false;
	if (!gpu_meshing_shadow_) return result;
	WtGpuMeshingShadowRequest request;
	if (!gpu_meshing_shadow_->pop(request)) return result;
	godot::Dictionary batch = wt_gpu_meshing_shadow_cell_batch(request.records);
	const godot::PackedFloat32Array densities = batch.get(
		"densities", godot::PackedFloat32Array()
	);
	result["status"] = "PASS";
	result["request_id"] = static_cast<std::int64_t>(request.request_id);
	result["identity"] = wt_gpu_meshing_shadow_identity(request, densities.size());
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
	if (!wt_parse_gpu_meshing_shadow_identity(identity_dictionary, identity)) {
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

godot::Dictionary
WorldTransvoxelTerrain::complete_gpu_meshing_publication_request(
	std::int64_t request_id,
	const godot::Dictionary &identity_dictionary,
	const godot::Array &gpu_cells,
	bool matched,
	const godot::String &error
) {
	godot::Dictionary result;
	result["schema"] =
		"world_transvoxel.gpu_meshing_publication_completion.v1";
	result["request_id"] = request_id;
	result["status"] = "UNKNOWN_REQUEST";
	result["accepted"] = false;
	result["published"] = false;
	result["publication_queued"] = false;
	result["gpu_cell_payload_used"] = false;
	result["native_chunk_finalization_used"] = false;
	result["exact_cpu_authority_match"] = false;
	result["cpu_collision_publication_unchanged"] = true;
	result["gpu_resident_render_publication"] = false;
	result["cpu_array_mesh_upload_required"] = true;
	if (!gpu_meshing_shadow_ || request_id <= 0) {
		result["error"] = "GPU publication request is invalid";
		return result;
	}
	WtGpuMeshingShadowIdentity identity;
	if (!wt_parse_gpu_meshing_shadow_identity(identity_dictionary, identity)) {
		result["status"] = "IDENTITY_MISMATCH";
		result["error"] = "GPU publication result identity is invalid";
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
	if (completion.status != WtGpuMeshingShadowCompletionStatus::Matched) {
		return result;
	}
	++gpu_meshing_publication_attempts_;
	if (!gpu_meshing_publication_enabled_) {
		++gpu_meshing_publication_application_rejections_;
		result["publication_status"] = "DISABLED";
		result["error"] = "GPU publication is not explicitly enabled";
		return result;
	}
	if (!completion.has_retained_request) {
		++gpu_meshing_publication_finalization_rejections_;
		result["publication_status"] = "MISSING_AUTHORITY";
		result["error"] = "GPU publication lost its retained authority request";
		return result;
	}
	std::shared_ptr<WtRenderPayload> payload;
	godot::String finalization_error;
	if (!build_gpu_cell_render_candidate(
			completion.retained_request,
			gpu_cells,
			payload,
			finalization_error
		)) {
		++gpu_meshing_publication_finalization_rejections_;
		result["publication_status"] = "FINALIZATION_REJECTED";
		result["error"] = finalization_error;
		return result;
	}
	result["gpu_cell_payload_used"] = true;
	result["native_chunk_finalization_used"] = true;
	result["exact_cpu_authority_match"] = true;
	WtChunkApplicationRecord application_record;
	if (!application_ || !application_->copy_record(
			payload->key, application_record
		) || application_record.generation != payload->generation ||
		!application_record.visual_required || !application_record.visual_ready) {
		++gpu_meshing_publication_stale_application_skips_;
		result["publication_status"] = "STALE_APPLICATION";
		result["error"] =
			"GPU-cell render no longer matches the applied visual generation";
		return result;
	}
	const WtApplicationStatus submit_status = application_->submit_render(payload);
	if (submit_status != WtApplicationStatus::Ok) {
		++gpu_meshing_publication_application_rejections_;
		result["publication_status"] = "QUEUE_REJECTED";
		result["error"] = "GPU-cell render publication queue rejected the payload";
		return result;
	}
	++gpu_meshing_publication_queued_;
	if (completion.retained_request.surface ==
			WtGpuMeshingShadowSurface::StaticWater) {
		++gpu_meshing_publication_water_queued_;
	} else {
		++gpu_meshing_publication_terrain_queued_;
	}
	result["publication_status"] = "QUEUED";
	result["publication_queued"] = true;
	result["publication_source"] = "GPU_CELL_CANDIDATE";
	result["surface"] = wt_gpu_meshing_shadow_surface_name(
		completion.retained_request.surface
	);
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
	result["superseded_queued_requests"] = static_cast<std::int64_t>(
		metrics.superseded_queued_requests
	);
	result["matched_results"] = static_cast<std::int64_t>(metrics.matched_results);
	result["mismatched_results"] = static_cast<std::int64_t>(metrics.mismatched_results);
	result["stale_results"] = static_cast<std::int64_t>(metrics.stale_results);
	result["unknown_results"] = static_cast<std::int64_t>(metrics.unknown_results);
	result["identity_mismatches"] = static_cast<std::int64_t>(metrics.identity_mismatches);
	result["cpu_render_authority"] = true;
	result["cpu_collision_authority"] = true;
	result["gpu_publication_enabled"] = gpu_meshing_publication_enabled_;
	result["gpu_resident_render_publication"] = false;
	result["gpu_publication_attempts"] = static_cast<std::int64_t>(
		gpu_meshing_publication_attempts_
	);
	result["gpu_publication_queued"] = static_cast<std::int64_t>(
		gpu_meshing_publication_queued_
	);
	result["gpu_publication_finalization_rejections"] =
		static_cast<std::int64_t>(
			gpu_meshing_publication_finalization_rejections_
		);
	result["gpu_publication_application_rejections"] =
		static_cast<std::int64_t>(
			gpu_meshing_publication_application_rejections_
		);
	result["gpu_publication_stale_application_skips"] =
		static_cast<std::int64_t>(
			gpu_meshing_publication_stale_application_skips_
		);
	result["gpu_publication_terrain_queued"] = static_cast<std::int64_t>(
		gpu_meshing_publication_terrain_queued_
	);
	result["gpu_publication_water_queued"] = static_cast<std::int64_t>(
		gpu_meshing_publication_water_queued_
	);
	return result;
}

} // namespace world_transvoxel
