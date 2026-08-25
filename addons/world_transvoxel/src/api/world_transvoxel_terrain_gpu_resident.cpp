#include "api/world_transvoxel_terrain.h"

#include "diagnostics/wt_gpu_meshing_godot_codec.h"
#include "diagnostics/wt_gpu_meshing_shadow.h"
#include "render/wt_godot_render_sink.h"
#include "services/wt_chunk_application.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace world_transvoxel {

bool WorldTransvoxelTerrain::begin_gpu_resident_render_publication(
	std::int64_t capacity
) {
	gpu_meshing_publication_enabled_ = false;
	gpu_resident_render_publication_enabled_ = false;
	gpu_resident_render_validation_attempts_ = 0;
	gpu_resident_render_validation_ready_ = 0;
	gpu_resident_render_validation_rejections_ = 0;
	gpu_resident_render_stale_skips_ = 0;
	gpu_resident_render_readiness_attempts_ = 0;
	gpu_resident_render_readiness_waits_ = 0;
	gpu_resident_render_readiness_ready_ = 0;
	gpu_resident_render_readiness_stale_ = 0;
	gpu_resident_render_activated_chunks_ = 0;
	gpu_resident_render_retired_chunks_ = 0;
	gpu_resident_render_reconciled_retires_ = 0;
	gpu_resident_render_restored_cpu_chunks_ = 0;
	if (render_sink_) {
		gpu_resident_render_restored_cpu_chunks_ +=
			render_sink_->restore_gpu_resident_replacements();
	}
	if (!gpu_meshing_shadow_ || !gpu_meshing_shadow_->begin(
			static_cast<std::size_t>(std::clamp<std::int64_t>(capacity, 0, 16))
		)) {
		return false;
	}
	gpu_resident_render_publication_enabled_ = true;
	return true;
}

void WorldTransvoxelTerrain::end_gpu_resident_render_publication() {
	gpu_resident_render_publication_enabled_ = false;
	if (gpu_meshing_shadow_) gpu_meshing_shadow_->end();
	if (render_sink_) {
		gpu_resident_render_restored_cpu_chunks_ +=
			render_sink_->restore_gpu_resident_replacements();
	}
}

godot::Dictionary WorldTransvoxelTerrain::pop_gpu_resident_render_request() {
	godot::Dictionary result;
	result["schema"] =
		"world_transvoxel.gpu_resident_render_request.v2";
	result["status"] = "EMPTY";
	result["gpu_resident_render_publication"] = true;
	result["cpu_render_visible_until_activation"] = true;
	result["cpu_collision_publication_unchanged"] = true;
	result["native_input_packing"] = true;
	result["cell_batch_exported"] = false;
	result["fallback_used"] = false;
	if (!gpu_resident_render_publication_enabled_ || !gpu_meshing_shadow_) {
		result["status"] = "DISABLED";
		return result;
	}
	WtGpuMeshingShadowRequest request;
	if (!gpu_meshing_shadow_->pop(request)) return result;
	const godot::Dictionary packed = wt_gpu_meshing_shadow_packed_input(request);
	if (packed.get("status", "FAIL") != "PASS") {
		WtGpuMeshingShadowIdentity identity;
		identity.key = request.job.key;
		identity.generation = request.job.generation;
		identity.source_revision = request.job.source_revision;
		identity.world_revision = request.job.world_revision;
		identity.transition_mask = request.transition_mask;
		identity.surface = request.surface;
		gpu_meshing_shadow_->reject_resident(
			request.request_id,
			identity,
			"Native GPU input packing failed"
		);
		++gpu_resident_render_validation_rejections_;
		result["status"] = "PACKING_FAILED";
		result["request_id"] = static_cast<std::int64_t>(request.request_id);
		result["error"] = packed.get("error", "Native GPU input packing failed");
		return result;
	}
	result["status"] = "PASS";
	result["request_id"] = static_cast<std::int64_t>(request.request_id);
	result["identity"] = wt_gpu_meshing_shadow_identity(
		request, packed.get("sample_count", 0)
	);
	result["gpu_input_buffers"] = packed.get("input_buffers", godot::Array());
	result["cell_count"] = packed.get("cell_count", 0);
	result["packed_byte_count"] = packed.get("packed_byte_count", 0);
	return result;
}

godot::Dictionary WorldTransvoxelTerrain::validate_gpu_resident_render_request(
	std::int64_t request_id,
	const godot::Dictionary &identity_dictionary
) {
	godot::Dictionary result;
	result["schema"] =
		"world_transvoxel.gpu_resident_render_validation.v1";
	result["request_id"] = request_id;
	result["status"] = "UNKNOWN_REQUEST";
	result["ready"] = false;
	result["request_accepted"] = false;
	result["cpu_render_publication_unchanged"] = true;
	result["cpu_collision_publication_unchanged"] = true;
	++gpu_resident_render_validation_attempts_;
	if (!gpu_resident_render_publication_enabled_ || !gpu_meshing_shadow_ ||
		request_id <= 0) {
		++gpu_resident_render_validation_rejections_;
		result["status"] = "DISABLED";
		result["error"] = "GPU resident publication is not enabled";
		return result;
	}
	WtGpuMeshingShadowIdentity identity;
	if (!wt_parse_gpu_meshing_shadow_identity(identity_dictionary, identity)) {
		++gpu_resident_render_validation_rejections_;
		result["status"] = "IDENTITY_MISMATCH";
		result["error"] = "GPU resident identity is invalid";
		return result;
	}
	const WtGpuMeshingResidentValidation validation =
		gpu_meshing_shadow_->validate_resident(
			static_cast<std::uint64_t>(request_id),
			identity,
			static_cast<std::uint64_t>(std::max<std::int64_t>(
				0, get_world_source_revision()
			)),
			static_cast<std::uint64_t>(std::max<std::int64_t>(
				0, get_world_revision()
			))
		);
	result["status"] = wt_gpu_meshing_resident_validation_status_name(
		validation.status
	);
	result["error"] = validation.error.c_str();
	if (validation.status == WtGpuMeshingResidentValidationStatus::Stale) {
		++gpu_resident_render_stale_skips_;
		return result;
	}
	if (validation.status != WtGpuMeshingResidentValidationStatus::Ready) {
		++gpu_resident_render_validation_rejections_;
		return result;
	}
	++gpu_resident_render_validation_ready_;
	result = get_gpu_resident_render_chunk_readiness(identity_dictionary);
	result["request_id"] = request_id;
	result["request_accepted"] = true;
	return result;
}

godot::Dictionary WorldTransvoxelTerrain::
get_gpu_resident_render_chunk_readiness(
	const godot::Dictionary &identity_dictionary
) {
	godot::Dictionary result;
	result["schema"] =
		"world_transvoxel.gpu_resident_render_readiness.v1";
	result["status"] = "STALE_APPLICATION";
	result["ready"] = false;
	result["cpu_render_publication_unchanged"] = true;
	result["cpu_collision_publication_unchanged"] = true;
	++gpu_resident_render_readiness_attempts_;
	WtGpuMeshingShadowIdentity identity;
	if (!gpu_resident_render_publication_enabled_ ||
		!wt_parse_gpu_meshing_shadow_identity(identity_dictionary, identity) ||
		identity.source_revision != static_cast<std::uint64_t>(
			std::max<std::int64_t>(0, get_world_source_revision())
		) || identity.world_revision != static_cast<std::uint64_t>(
			std::max<std::int64_t>(0, get_world_revision())
		)) {
		++gpu_resident_render_readiness_stale_;
		++gpu_resident_render_stale_skips_;
		result["error"] = "GPU resident chunk revision is stale";
		return result;
	}
	WtChunkApplicationRecord record;
	if (!application_ || !render_sink_ || !application_->copy_record(
			identity.key, record
		) || record.generation != identity.generation ||
		!record.visual_required) {
		++gpu_resident_render_readiness_stale_;
		++gpu_resident_render_stale_skips_;
		result["error"] =
			"GPU resident geometry no longer matches the CPU chunk generation";
		return result;
	}
	if (!record.visual_ready ||
		record.visual_generation != identity.generation) {
		++gpu_resident_render_readiness_waits_;
		result["status"] = "WAITING_APPLICATION";
		result["error"] =
			"GPU resident geometry is waiting for CPU visual readiness";
		return result;
	}
	if (!render_sink_->can_set_gpu_resident_replacement(
			identity.key, identity.generation, identity.transition_mask
		)) {
		if (render_sink_->staged_generation(identity.key) ==
				identity.generation) {
			++gpu_resident_render_readiness_waits_;
			result["status"] = "WAITING_APPLICATION";
			result["error"] =
				"GPU resident geometry is waiting for staged CPU publication";
			return result;
		}
		++gpu_resident_render_readiness_stale_;
		++gpu_resident_render_stale_skips_;
		result["error"] =
			"GPU resident geometry no longer matches the applied CPU visual";
		return result;
	}
	++gpu_resident_render_readiness_ready_;
	result["status"] = "READY";
	result["ready"] = true;
	result["error"] = "";
	return result;
}

godot::Dictionary WorldTransvoxelTerrain::reject_gpu_resident_render_request(
	std::int64_t request_id,
	const godot::Dictionary &identity_dictionary,
	const godot::String &error
) {
	godot::Dictionary result;
	result["schema"] =
		"world_transvoxel.gpu_resident_render_validation.v1";
	result["request_id"] = request_id;
	result["status"] = "UNKNOWN_REQUEST";
	result["ready"] = false;
	if (!gpu_meshing_shadow_ || request_id <= 0) {
		result["error"] = "GPU resident rejection request is invalid";
		return result;
	}
	WtGpuMeshingShadowIdentity identity;
	if (!wt_parse_gpu_meshing_shadow_identity(identity_dictionary, identity)) {
		result["status"] = "IDENTITY_MISMATCH";
		result["error"] = "Rejected GPU resident identity is invalid";
		return result;
	}
	const WtGpuMeshingResidentValidation validation =
		gpu_meshing_shadow_->reject_resident(
			static_cast<std::uint64_t>(request_id),
			identity,
			std::string(error.utf8().get_data())
		);
	result["status"] = wt_gpu_meshing_resident_validation_status_name(
		validation.status
	);
	result["error"] = validation.error.c_str();
	++gpu_resident_render_validation_rejections_;
	return result;
}

godot::Dictionary WorldTransvoxelTerrain::set_gpu_resident_render_chunk_active(
	const godot::Array &identity_dictionaries,
	bool active
) {
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.gpu_resident_chunk_activation.v1";
	result["status"] = "REJECTED";
	result["active"] = false;
	result["cpu_collision_publication_unchanged"] = true;
	if (identity_dictionaries.is_empty() || identity_dictionaries.size() > 2 ||
		!render_sink_) {
		result["error"] = "GPU resident chunk surface inventory is invalid";
		return result;
	}
	WtGpuMeshingShadowIdentity chunk_identity;
	bool have_chunk_identity = false;
	bool have_terrain = false;
	bool have_water = false;
	bool water_expected = false;
	for (std::int64_t index = 0; index < identity_dictionaries.size(); ++index) {
		const godot::Dictionary dictionary = identity_dictionaries[index];
		WtGpuMeshingShadowIdentity identity;
		if (!wt_parse_gpu_meshing_shadow_identity(dictionary, identity)) {
			result["error"] = "GPU resident chunk contains an invalid identity";
			return result;
		}
		if (!have_chunk_identity) {
			chunk_identity = identity;
			have_chunk_identity = true;
			water_expected = dictionary.get(
				"static_water_surface_expected", false
			);
		} else if (identity.key != chunk_identity.key ||
			identity.generation != chunk_identity.generation ||
			identity.source_revision != chunk_identity.source_revision ||
			identity.world_revision != chunk_identity.world_revision ||
			identity.transition_mask != chunk_identity.transition_mask ||
			static_cast<bool>(dictionary.get(
				"static_water_surface_expected", false
			)) != water_expected) {
			result["error"] = "GPU resident chunk surfaces do not share identity";
			return result;
		}
		if (identity.surface == WtGpuMeshingShadowSurface::Terrain) {
			if (have_terrain) {
				result["error"] = "GPU resident chunk repeats terrain surface";
				return result;
			}
			have_terrain = true;
		} else {
			if (have_water) {
				result["error"] = "GPU resident chunk repeats water surface";
				return result;
			}
			have_water = true;
		}
	}
	if (!have_terrain || (active && water_expected != have_water)) {
		result["error"] = "GPU resident chunk lacks its complete surface set";
		return result;
	}
	if (!active) {
		const bool restored = render_sink_->set_gpu_resident_replacement(
			chunk_identity.key,
			chunk_identity.generation,
			chunk_identity.transition_mask,
			false
		);
		if (restored) ++gpu_resident_render_restored_cpu_chunks_;
		++gpu_resident_render_retired_chunks_;
		result["status"] = "RETIRED";
		result["cpu_visual_restored"] = restored;
		return result;
	}
	if (!gpu_resident_render_publication_enabled_ ||
		chunk_identity.source_revision != static_cast<std::uint64_t>(
			std::max<std::int64_t>(0, get_world_source_revision())
		) || chunk_identity.world_revision != static_cast<std::uint64_t>(
			std::max<std::int64_t>(0, get_world_revision())
		)) {
		result["status"] = "STALE";
		result["error"] = "GPU resident chunk revision became stale";
		return result;
	}
	WtChunkApplicationRecord record;
	if (!application_ || !application_->copy_record(chunk_identity.key, record) ||
		record.generation != chunk_identity.generation ||
		record.visual_generation != chunk_identity.generation ||
		!record.visual_required || !record.visual_ready ||
		!render_sink_->set_gpu_resident_replacement(
			chunk_identity.key,
			chunk_identity.generation,
			chunk_identity.transition_mask,
			true
		)) {
		result["status"] = "STALE_APPLICATION";
		result["error"] = "GPU resident chunk cannot replace the CPU visual";
		return result;
	}
	++gpu_resident_render_activated_chunks_;
	result["status"] = "ACTIVE";
	result["active"] = true;
	result["cpu_visual_hidden"] = true;
	result["surface_count"] = identity_dictionaries.size();
	result["error"] = "";
	return result;
}

godot::Dictionary WorldTransvoxelTerrain::reconcile_gpu_resident_render_chunks(
	const godot::Array &terrain_identity_dictionaries
) {
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.gpu_resident_reconciliation.v1";
	result["status"] = "PASS";
	result["cpu_collision_publication_unchanged"] = true;
	godot::Array retire;
	for (std::int64_t index = 0;
			index < terrain_identity_dictionaries.size(); ++index) {
		const godot::Dictionary dictionary =
			terrain_identity_dictionaries[index];
		WtGpuMeshingShadowIdentity identity;
		bool valid = gpu_resident_render_publication_enabled_ &&
			wt_parse_gpu_meshing_shadow_identity(dictionary, identity) &&
			identity.surface == WtGpuMeshingShadowSurface::Terrain &&
			identity.source_revision == static_cast<std::uint64_t>(
				std::max<std::int64_t>(0, get_world_source_revision())
			);
		WtChunkApplicationRecord record;
		valid = valid && application_ && render_sink_ &&
			application_->copy_record(identity.key, record) &&
			record.generation == identity.generation &&
			record.visual_generation == identity.generation &&
			record.visual_required && record.visual_ready;
		if (valid && !render_sink_->gpu_resident_replacement_matches(
				identity.key, identity.generation, identity.transition_mask
			)) {
			valid = render_sink_->set_gpu_resident_replacement(
				identity.key, identity.generation, identity.transition_mask, true
			);
		}
		if (valid) continue;
		if (render_sink_) {
			render_sink_->set_gpu_resident_replacement(
				identity.key, identity.generation, identity.transition_mask, false
			);
		}
		retire.push_back(dictionary);
	}
	gpu_resident_render_reconciled_retires_ +=
		static_cast<std::uint64_t>(retire.size());
	result["retire"] = retire;
	result["retire_count"] = retire.size();
	result["checked_count"] = terrain_identity_dictionaries.size();
	return result;
}

godot::Dictionary WorldTransvoxelTerrain::get_gpu_resident_render_metrics() const {
	const WtGpuMeshingShadowMetrics queue_metrics = gpu_meshing_shadow_ ?
		gpu_meshing_shadow_->metrics() : WtGpuMeshingShadowMetrics{};
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.gpu_resident_render_metrics.v1";
	result["enabled"] = gpu_resident_render_publication_enabled_;
	result["capacity"] = static_cast<std::int64_t>(queue_metrics.capacity);
	result["queued_requests"] = static_cast<std::int64_t>(
		queue_metrics.queued_requests
	);
	result["in_flight_requests"] = static_cast<std::int64_t>(
		queue_metrics.in_flight_requests
	);
	result["captured_requests"] = static_cast<std::int64_t>(
		queue_metrics.captured_requests
	);
	result["capacity_rejections"] = static_cast<std::int64_t>(
		queue_metrics.capacity_rejections
	);
	result["superseded_queued_requests"] = static_cast<std::int64_t>(
		queue_metrics.superseded_queued_requests
	);
	result["reserved_capture_slots"] = static_cast<std::int64_t>(
		queue_metrics.reserved_capture_slots
	);
	result["capture_reservation_attempts"] = static_cast<std::int64_t>(
		queue_metrics.capture_reservation_attempts
	);
	result["capture_reservation_rejections"] = static_cast<std::int64_t>(
		queue_metrics.capture_reservation_rejections
	);
	result["reserved_captures"] = static_cast<std::int64_t>(
		queue_metrics.reserved_captures
	);
	result["released_capture_slots"] = static_cast<std::int64_t>(
		queue_metrics.released_capture_slots
	);
	result["validation_attempts"] = static_cast<std::int64_t>(
		gpu_resident_render_validation_attempts_
	);
	result["validation_ready"] = static_cast<std::int64_t>(
		gpu_resident_render_validation_ready_
	);
	result["validation_rejections"] = static_cast<std::int64_t>(
		gpu_resident_render_validation_rejections_
	);
	result["stale_skips"] = static_cast<std::int64_t>(
		gpu_resident_render_stale_skips_
	);
	result["readiness_attempts"] = static_cast<std::int64_t>(
		gpu_resident_render_readiness_attempts_
	);
	result["readiness_waits"] = static_cast<std::int64_t>(
		gpu_resident_render_readiness_waits_
	);
	result["readiness_ready"] = static_cast<std::int64_t>(
		gpu_resident_render_readiness_ready_
	);
	result["readiness_stale"] = static_cast<std::int64_t>(
		gpu_resident_render_readiness_stale_
	);
	result["activated_chunks"] = static_cast<std::int64_t>(
		gpu_resident_render_activated_chunks_
	);
	result["retired_chunks"] = static_cast<std::int64_t>(
		gpu_resident_render_retired_chunks_
	);
	result["reconciled_retires"] = static_cast<std::int64_t>(
		gpu_resident_render_reconciled_retires_
	);
	result["restored_cpu_chunks"] = static_cast<std::int64_t>(
		gpu_resident_render_restored_cpu_chunks_
	);
	result["cpu_collision_authority"] = true;
	return result;
}

} // namespace world_transvoxel
