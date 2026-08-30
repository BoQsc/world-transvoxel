#include "api/world_transvoxel_terrain.h"

#include "diagnostics/wt_gpu_meshing_godot_codec.h"
#include "diagnostics/wt_gpu_meshing_shadow.h"
#include "render/wt_godot_render_sink.h"
#include "services/wt_chunk_application.h"
#include "services/wt_chunk_publication_policy.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace world_transvoxel {

namespace {

struct ParsedGpuChunkInventory {
	WtGpuMeshingShadowIdentity identity;
	bool water_expected = false;
	bool have_terrain = false;
	bool have_water = false;
};

bool parse_gpu_chunk_inventory(
	const godot::Array &identity_dictionaries,
	ParsedGpuChunkInventory &inventory,
	std::string &error
) {
	if (identity_dictionaries.is_empty() || identity_dictionaries.size() > 2) {
		error = "GPU resident chunk surface inventory is invalid";
		return false;
	}
	bool have_chunk_identity = false;
	for (std::int64_t index = 0; index < identity_dictionaries.size(); ++index) {
		const godot::Dictionary dictionary = identity_dictionaries[index];
		WtGpuMeshingShadowIdentity identity;
		if (!wt_parse_gpu_meshing_shadow_identity(dictionary, identity)) {
			error = "GPU resident chunk contains an invalid identity";
			return false;
		}
		const bool expected = dictionary.get(
			"static_water_surface_expected", false
		);
		if (!have_chunk_identity) {
			inventory.identity = identity;
			inventory.water_expected = expected;
			have_chunk_identity = true;
		} else if (identity.key != inventory.identity.key ||
			identity.generation != inventory.identity.generation ||
			identity.source_revision != inventory.identity.source_revision ||
			identity.world_revision != inventory.identity.world_revision ||
			identity.transition_mask != inventory.identity.transition_mask ||
			expected != inventory.water_expected) {
			error = "GPU resident chunk surfaces do not share identity";
			return false;
		}
		if (identity.surface == WtGpuMeshingShadowSurface::Terrain) {
			if (inventory.have_terrain) {
				error = "GPU resident chunk repeats terrain surface";
				return false;
			}
			inventory.have_terrain = true;
		} else {
			if (inventory.have_water) {
				error = "GPU resident chunk repeats water surface";
				return false;
			}
			inventory.have_water = true;
		}
	}
	if (!inventory.have_terrain || inventory.water_expected != inventory.have_water) {
		error = "GPU resident chunk lacks its complete surface set";
		return false;
	}
	return true;
}

godot::Dictionary gpu_cohort_member(
	const WtChunkApplicationRecord &record
) {
	godot::Dictionary result;
	result["page_x"] = record.key.x;
	result["page_y"] = record.key.y;
	result["page_z"] = record.key.z;
	result["lod"] = static_cast<std::int64_t>(record.key.lod);
	result["generation"] = static_cast<std::int64_t>(record.generation.value);
	result["transition_mask"] = static_cast<std::int64_t>(
		record.external_visual_transition_mask
	);
	return result;
}

godot::Dictionary gpu_cohort_key(const WtChunkKey &key) {
	godot::Dictionary result;
	result["page_x"] = key.x;
	result["page_y"] = key.y;
	result["page_z"] = key.z;
	result["lod"] = static_cast<std::int64_t>(key.lod);
	return result;
}

godot::Array gpu_cohort_keys(const std::vector<WtChunkKey> &keys) {
	godot::Array result;
	for (const WtChunkKey &key : keys) result.push_back(gpu_cohort_key(key));
	return result;
}

} // namespace

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
	gpu_resident_render_prepared_chunks_ = 0;
	gpu_resident_render_activation_cohorts_ = 0;
	gpu_resident_render_activation_cohort_chunks_ = 0;
	gpu_resident_render_activated_chunks_ = 0;
	gpu_resident_render_retired_chunks_ = 0;
	gpu_resident_render_reconciled_retires_ = 0;
	gpu_resident_render_restored_cpu_chunks_ = 0;
	if (render_sink_) {
		gpu_resident_render_restored_cpu_chunks_ +=
			render_sink_->restore_gpu_resident_replacements();
	}
	if (!gpu_meshing_shadow_ || !gpu_meshing_shadow_->begin(
			static_cast<std::size_t>(std::clamp<std::int64_t>(capacity, 0, 16)),
			true,
			WtGpuMeshingCaptureStage::PreMeshField
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
		"world_transvoxel.gpu_resident_render_request.v6";
	result["position_space"] = "world";
	result["input_stage"] = "pre_mesh_field";
	result["status"] = "EMPTY";
	result["gpu_resident_render_publication"] = true;
	result["cpu_render_visible_until_activation"] = false;
	result["cpu_visual_mesh_omitted"] = false;
	result["cpu_collision_publication_unchanged"] = true;
	result["native_input_packing"] = true;
	result["cpu_topology_input_dependency"] = false;
	result["cpu_field_sampling"] = false;
	result["gpu_density_field_generation"] = true;
	result["gpu_material_field_generation"] = true;
	result["gpu_page_lattice_input"] = true;
	result["gpu_transvoxel_extraction"] = true;
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
	result["cpu_visual_mesh_omitted"] = request.cpu_visual_mesh_omitted;
	result["cpu_render_visible_until_activation"] =
		!request.cpu_visual_mesh_omitted;
	result["identity"] = wt_gpu_meshing_shadow_identity(
		request, packed.get("sample_count", 0)
	);
	result["gpu_input_buffers"] = packed.get("input_buffers", godot::Array());
	result["cell_count"] = packed.get("cell_count", 0);
	result["proven_empty"] = packed.get("proven_empty", false);
	result["bounds_min"] = packed.get("bounds_min", godot::Vector3());
	result["bounds_max"] = packed.get("bounds_max", godot::Vector3());
	result["packed_byte_count"] = packed.get("packed_byte_count", 0);
	result["page_count"] = packed.get("page_count", 0);
	result["input_stage"] = wt_gpu_meshing_capture_stage_name(
		request.capture_stage
	);
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
		)) {
		++gpu_resident_render_readiness_stale_;
		++gpu_resident_render_stale_skips_;
		result["error"] = "GPU resident chunk revision is stale";
		return result;
	}
	if (!application_ || !render_sink_) {
		++gpu_resident_render_readiness_stale_;
		++gpu_resident_render_stale_skips_;
		result["error"] = "GPU resident application services are unavailable";
		return result;
	}
	WtChunkApplicationRecord record;
	if (!application_->copy_record(identity.key, record)) {
		++gpu_resident_render_readiness_waits_;
		result["status"] = "WAITING_APPLICATION";
		result["error"] =
			"GPU resident geometry is waiting for its CPU application record";
		return result;
	}
	if (record.generation != identity.generation || !record.visual_required) {
		++gpu_resident_render_readiness_stale_;
		++gpu_resident_render_stale_skips_;
		result["error"] =
			"GPU resident geometry no longer matches the CPU chunk generation";
		return result;
	}
	if (record.visual_generation != identity.generation ||
		(!record.visual_ready &&
			!record.external_visual_activation_required)) {
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

godot::Dictionary WorldTransvoxelTerrain::prepare_gpu_resident_render_chunk(
	const godot::Array &identity_dictionaries
) {
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.gpu_resident_chunk_preparation.v1";
	result["status"] = "REJECTED";
	result["prepared"] = false;
	result["cpu_collision_publication_unchanged"] = true;
	ParsedGpuChunkInventory inventory;
	std::string error;
	if (!parse_gpu_chunk_inventory(identity_dictionaries, inventory, error)) {
		result["error"] = error.c_str();
		return result;
	}
	if (!gpu_resident_render_publication_enabled_ || !application_ || !render_sink_ ||
		inventory.identity.source_revision != static_cast<std::uint64_t>(
			std::max<std::int64_t>(0, get_world_source_revision())
		)) {
		result["status"] = "STALE";
		result["error"] = "GPU resident chunk revision became stale";
		return result;
	}
	WtChunkApplicationRecord record;
	if (!application_->copy_record(inventory.identity.key, record) ||
		record.generation != inventory.identity.generation ||
		record.visual_generation != inventory.identity.generation ||
		!record.visual_required || !record.external_visual_activation_required ||
		!render_sink_->can_set_gpu_resident_replacement(
			inventory.identity.key,
			inventory.identity.generation,
			inventory.identity.transition_mask
		)) {
		result["status"] = "STALE_APPLICATION";
		result["error"] = "GPU resident chunk cannot enter prepared publication";
		return result;
	}
	const WtApplicationStatus prepared =
		application_->confirm_external_visual_prepared(
			inventory.identity.key,
			inventory.identity.generation,
			inventory.identity.transition_mask
		);
	if (prepared != WtApplicationStatus::Ok &&
		prepared != WtApplicationStatus::AlreadyCurrent) {
		result["status"] = "STALE_APPLICATION";
		result["error"] = "GPU resident preparation did not match application state";
		return result;
	}
	if (prepared == WtApplicationStatus::Ok) {
		++gpu_resident_render_prepared_chunks_;
	}
	result["status"] = "PREPARED";
	result["prepared"] = true;
	result["error"] = "";
	return result;
}

godot::Dictionary WorldTransvoxelTerrain::
get_gpu_resident_render_activation_cohort(
	const godot::Dictionary &identity_dictionary
) {
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.gpu_resident_activation_cohort.v1";
	result["status"] = "STALE_APPLICATION";
	result["ready"] = false;
	result["regional"] = false;
	result["chunks"] = godot::Array();
	result["retirements"] = godot::Array();
	result["cpu_collision_publication_unchanged"] = true;
	WtGpuMeshingShadowIdentity identity;
	if (!gpu_resident_render_publication_enabled_ || !application_ || !render_sink_ ||
		!wt_parse_gpu_meshing_shadow_identity(identity_dictionary, identity) ||
		identity.surface != WtGpuMeshingShadowSurface::Terrain) {
		result["error"] = "GPU resident cohort seed is invalid";
		return result;
	}
	WtChunkApplicationRecord seed_record;
	if (!application_->copy_record(identity.key, seed_record) ||
		seed_record.generation != identity.generation ||
		seed_record.visual_generation != identity.generation ||
		seed_record.external_visual_transition_mask != identity.transition_mask) {
		result["error"] = "GPU resident cohort seed became stale";
		return result;
	}
	result["open_viewer_plan_publications"] = static_cast<std::int64_t>(
		open_viewer_plan_publications_
	);
	result["pending_replacement_count"] = static_cast<std::int64_t>(
		pending_chunk_replacements_.size()
	);
	result["ready_staged_replacement_count"] = static_cast<std::int64_t>(
		ready_staged_chunk_replacements_.size()
	);
	result["pending_retirement_count"] = static_cast<std::int64_t>(
		pending_chunk_retirements_.size()
	);
	result["seed_independently_publishable"] = std::binary_search(
		independently_publishable_chunk_replacements_.begin(),
		independently_publishable_chunk_replacements_.end(),
		identity.key
	);
	std::vector<WtChunkKey> replacements { identity.key };
	std::vector<WtChunkKey> retirements;
	std::size_t retirement_count = 0;
	const bool regional = seed_record.staged_replacement &&
		wt_chunk_replacement_requires_regional_publication(
			identity.key, pending_chunk_retirements_
		);
	if (regional) {
		std::vector<WtChunkKey> candidates = pending_chunk_replacements_;
		candidates.insert(
			candidates.end(),
			ready_staged_chunk_replacements_.begin(),
			ready_staged_chunk_replacements_.end()
		);
		std::sort(candidates.begin(), candidates.end());
		candidates.erase(
			std::unique(candidates.begin(), candidates.end()), candidates.end()
		);
		WtChunkPublicationRegion region;
		const bool region_built = wt_build_chunk_publication_region(
			identity.key, candidates, pending_chunk_retirements_, region
		);
		const bool complete_coverage = region_built &&
			publication_region_has_complete_authoritative_coverage(region);
		result["candidate_count"] = static_cast<std::int64_t>(candidates.size());
		result["region_built"] = region_built;
		result["region_replacement_count"] = static_cast<std::int64_t>(
			region.replacements.size()
		);
		result["region_retirement_count"] = static_cast<std::int64_t>(
			region.retirements.size()
		);
		result["complete_coverage"] = complete_coverage;
		if (!region_built) {
			// The seed left both replacement sets before activation. Waiting cannot
			// make this generation current again; the consumer must discard it and
			// accept the authority's replacement request.
			result["candidate_replacements"] = gpu_cohort_keys(candidates);
			result["status"] = "STALE_APPLICATION";
			result["error"] =
				"GPU resident regional replacement seed is no longer pending";
			return result;
		}
		if (!complete_coverage) {
			result["candidate_replacements"] = gpu_cohort_keys(candidates);
			result["region_replacements"] = gpu_cohort_keys(region.replacements);
			result["region_retirements"] = gpu_cohort_keys(region.retirements);
			result["status"] = "WAITING_COHORT";
			result["error"] = "GPU resident regional replacement is incomplete";
			return result;
		}
		replacements = std::move(region.replacements);
		retirements = std::move(region.retirements);
		retirement_count = retirements.size();
	}
	godot::Array chunks;
	std::int64_t activation_required_count = 0;
	std::int64_t retained_active_count = 0;
	std::vector<WtChunkApplicationRecord> missing_records;
	WtChunkKey first_waiting_key;
	WtChunkApplicationRecord first_waiting_record;
	bool has_waiting_member = false;
	bool first_waiting_record_present = false;
	for (const WtChunkKey &key : replacements) {
		WtChunkApplicationRecord record;
		const bool record_present = application_->copy_record(key, record);
		const bool generation_matches = record_present &&
			record.visual_generation == record.generation;
		const bool sink_can_set = record_present &&
			render_sink_->can_set_gpu_resident_replacement(
				key, record.generation, record.external_visual_transition_mask
			);
		const bool sink_matches = record_present &&
			render_sink_->gpu_resident_replacement_matches(
				key, record.generation, record.external_visual_transition_mask
			);
		const bool prepared_member = record_present && record.visual_required &&
			generation_matches &&
			record.external_visual_activation_required &&
			record.external_visual_prepared && sink_can_set;
		const bool retained_active_member = record_present &&
			record.visual_required && record.visual_ready &&
			generation_matches && !record.external_visual_activation_required &&
			sink_matches;
		if (!prepared_member && !retained_active_member) {
			if (!has_waiting_member) {
				first_waiting_key = key;
				first_waiting_record = record;
				first_waiting_record_present = record_present;
				has_waiting_member = true;
			}
			if (record_present && record.generation.value != 0 &&
				record.visual_required) {
				missing_records.push_back(record);
			}
			continue;
		}
		godot::Dictionary member = gpu_cohort_member(record);
		member["activation_required"] = prepared_member;
		chunks.push_back(member);
		activation_required_count += prepared_member ? 1 : 0;
		retained_active_count += retained_active_member ? 1 : 0;
	}
	if (has_waiting_member) {
		request_visibility_coverage_priority_batch(
			missing_records,
			replacements.size(),
			retirement_count
		);
		result["status"] = "WAITING_COHORT";
		result["error"] = "GPU resident replacement cohort is not fully prepared";
		result["waiting_member"] = first_waiting_record_present ?
			gpu_cohort_member(first_waiting_record) :
			gpu_cohort_key(first_waiting_key);
		result["waiting_member_record_present"] = first_waiting_record_present;
		result["waiting_member_visual_required"] =
			first_waiting_record.visual_required;
		result["waiting_member_external_activation_required"] =
			first_waiting_record.external_visual_activation_required;
		result["waiting_member_external_prepared"] =
			first_waiting_record.external_visual_prepared;
		result["waiting_member_visual_ready"] =
			first_waiting_record.visual_ready;
		result["waiting_member_generation_matches"] =
			first_waiting_record_present &&
			first_waiting_record.visual_generation ==
				first_waiting_record.generation;
		result["waiting_member_sink_can_set"] = first_waiting_record_present &&
			render_sink_->can_set_gpu_resident_replacement(
				first_waiting_record.key,
				first_waiting_record.generation,
				first_waiting_record.external_visual_transition_mask
			);
		result["waiting_member_sink_matches"] = first_waiting_record_present &&
			render_sink_->gpu_resident_replacement_matches(
				first_waiting_record.key,
				first_waiting_record.generation,
				first_waiting_record.external_visual_transition_mask
			);
		result["priority_requested_member_count"] =
			static_cast<std::int64_t>(missing_records.size());
		return result;
	}
	result["status"] = "READY";
	result["ready"] = true;
	result["regional"] = regional;
	result["chunks"] = chunks;
	result["retirements"] = gpu_cohort_keys(retirements);
	result["replacement_count"] = chunks.size();
	result["activation_required_count"] = activation_required_count;
	result["retained_active_count"] = retained_active_count;
	result["error"] = "";
	return result;
}

godot::Dictionary WorldTransvoxelTerrain::activate_gpu_resident_render_cohort(
	const godot::Array &chunk_surface_inventories,
	const godot::Dictionary &authoritative_seed_dictionary
) {
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.gpu_resident_cohort_activation.v1";
	result["status"] = "REJECTED";
	result["active"] = false;
	result["retirements"] = godot::Array();
	result["cpu_collision_publication_unchanged"] = true;
	if (!gpu_resident_render_publication_enabled_ || !application_ || !render_sink_ ||
		chunk_surface_inventories.is_empty() ||
		chunk_surface_inventories.size() > 4096) {
		result["error"] = "GPU resident activation cohort is invalid";
		return result;
	}
	std::vector<ParsedGpuChunkInventory> inventory_pool;
	inventory_pool.reserve(chunk_surface_inventories.size());
	for (std::int64_t index = 0;
			index < chunk_surface_inventories.size(); ++index) {
		const godot::Array identities = chunk_surface_inventories[index];
		ParsedGpuChunkInventory inventory;
		std::string error;
		if (!parse_gpu_chunk_inventory(identities, inventory, error)) {
			result["error"] = error.c_str();
			return result;
		}
		if (inventory.identity.source_revision != static_cast<std::uint64_t>(
				std::max<std::int64_t>(0, get_world_source_revision())
			)) {
			result["status"] = "STALE";
			result["error"] = "GPU resident activation cohort revision became stale";
			return result;
		}
		if (std::any_of(
				inventory_pool.begin(), inventory_pool.end(),
				[&inventory, &authoritative_seed_dictionary](
					const ParsedGpuChunkInventory &existing
				) {
					if (existing.identity.key != inventory.identity.key) return false;
					return authoritative_seed_dictionary.is_empty() ||
						existing.identity.generation == inventory.identity.generation;
				}
			)) {
			result["error"] = "GPU resident activation cohort repeats a chunk";
			return result;
		}
		inventory_pool.push_back(inventory);
	}
	std::vector<ParsedGpuChunkInventory> inventories;
	std::vector<WtChunkKey> authoritative_retirements;
	bool authority_selected = false;
	if (!authoritative_seed_dictionary.is_empty()) {
		WtGpuMeshingShadowIdentity seed;
		if (!wt_parse_gpu_meshing_shadow_identity(
				authoritative_seed_dictionary, seed
			) || seed.surface != WtGpuMeshingShadowSurface::Terrain) {
			result["error"] = "GPU resident authoritative cohort seed is invalid";
			return result;
		}
		std::vector<WtChunkKey> selected_keys { seed.key };
		if (wt_chunk_replacement_requires_regional_publication(
				seed.key, pending_chunk_retirements_
			)) {
			std::vector<WtChunkKey> candidates = pending_chunk_replacements_;
			candidates.insert(
				candidates.end(),
				ready_staged_chunk_replacements_.begin(),
				ready_staged_chunk_replacements_.end()
			);
			std::sort(candidates.begin(), candidates.end());
			candidates.erase(
				std::unique(candidates.begin(), candidates.end()), candidates.end()
			);
			WtChunkPublicationRegion region;
			const bool region_built = wt_build_chunk_publication_region(
				seed.key, candidates, pending_chunk_retirements_, region
			);
			if (!region_built) {
				result["status"] = "STALE_APPLICATION";
				result["error"] =
					"GPU resident authoritative cohort seed is no longer pending";
				return result;
			}
			if (!publication_region_has_complete_authoritative_coverage(region)) {
				result["status"] = "WAITING_COHORT";
				result["error"] =
					"GPU resident authoritative publication region is incomplete";
				return result;
			}
			selected_keys = std::move(region.replacements);
			authoritative_retirements = std::move(region.retirements);
		}
		for (const WtChunkKey &key : selected_keys) {
			WtChunkApplicationRecord record;
			if (!application_->copy_record(key, record)) {
				result["status"] = "WAITING_COHORT";
				result["error"] =
					"GPU resident authoritative publication member has no application record";
				result["waiting_member"] = gpu_cohort_key(key);
				return result;
			}
			const auto inventory = std::find_if(
				inventory_pool.begin(), inventory_pool.end(),
				[&key, &record](const ParsedGpuChunkInventory &candidate) {
					return candidate.identity.key == key &&
						candidate.identity.generation == record.generation;
				}
			);
			if (inventory == inventory_pool.end()) {
				result["status"] = "WAITING_COHORT";
				result["error"] =
					"GPU resident authoritative publication member is not prepared";
				result["waiting_member"] = gpu_cohort_key(key);
				return result;
			}
			inventories.push_back(*inventory);
		}
		authority_selected = true;
	} else {
		inventories = std::move(inventory_pool);
	}
	std::vector<bool> activation_required;
	activation_required.reserve(inventories.size());
	godot::Array activated_chunks;
	for (const ParsedGpuChunkInventory &inventory : inventories) {
		WtChunkApplicationRecord record;
		const bool record_present =
			application_->copy_record(inventory.identity.key, record);
		const bool prepared_member = record_present &&
			record.generation == inventory.identity.generation &&
			record.visual_generation == inventory.identity.generation &&
			record.visual_required && record.external_visual_activation_required &&
			record.external_visual_prepared &&
			record.external_visual_transition_mask == inventory.identity.transition_mask &&
			render_sink_->can_set_gpu_resident_replacement(
				inventory.identity.key,
				inventory.identity.generation,
				inventory.identity.transition_mask
			);
		const bool retained_active_member = record_present &&
			record.generation == inventory.identity.generation &&
			record.visual_generation == inventory.identity.generation &&
			record.visual_required && record.visual_ready &&
			!record.external_visual_activation_required &&
			render_sink_->gpu_resident_replacement_matches(
				inventory.identity.key,
				inventory.identity.generation,
				inventory.identity.transition_mask
			);
		if (!prepared_member && !retained_active_member) {
			result["status"] = "STALE_APPLICATION";
			result["error"] = "GPU resident activation cohort became stale";
			return result;
		}
		godot::Dictionary member = gpu_cohort_member(record);
		member["activation_required"] = prepared_member;
		activated_chunks.push_back(member);
		activation_required.push_back(prepared_member);
	}
	std::vector<WtChunkKey> cohort_keys;
	cohort_keys.reserve(inventories.size());
	for (const ParsedGpuChunkInventory &inventory : inventories) {
		cohort_keys.push_back(inventory.identity.key);
	}
	std::sort(cohort_keys.begin(), cohort_keys.end());
	const auto regional_seed = std::find_if(
		cohort_keys.begin(), cohort_keys.end(),
		[this](const WtChunkKey &key) {
			return wt_chunk_replacement_requires_regional_publication(
				key, pending_chunk_retirements_
			);
		}
	);
	if (!authority_selected && regional_seed != cohort_keys.end()) {
		std::vector<WtChunkKey> candidates = pending_chunk_replacements_;
		candidates.insert(
			candidates.end(),
			ready_staged_chunk_replacements_.begin(),
			ready_staged_chunk_replacements_.end()
		);
		std::sort(candidates.begin(), candidates.end());
		candidates.erase(
			std::unique(candidates.begin(), candidates.end()), candidates.end()
		);
		WtChunkPublicationRegion region;
		if (!wt_build_chunk_publication_region(
				*regional_seed, candidates, pending_chunk_retirements_, region
			) || !publication_region_has_complete_authoritative_coverage(region) ||
			region.replacements != cohort_keys) {
			result["status"] = "STALE_APPLICATION";
			result["error"] = "GPU resident activation cohort is not the authoritative region";
			result["regional_seed"] = gpu_cohort_key(*regional_seed);
			result["submitted_replacements"] = gpu_cohort_keys(cohort_keys);
			result["authoritative_replacements"] = gpu_cohort_keys(
				region.replacements
			);
			result["authoritative_retirements"] = gpu_cohort_keys(
				region.retirements
			);
			result["candidate_replacements"] = gpu_cohort_keys(candidates);
			result["pending_retirements"] = gpu_cohort_keys(
				pending_chunk_retirements_
			);
			result["open_viewer_plan_publications"] = static_cast<std::int64_t>(
				open_viewer_plan_publications_
			);
			return result;
		}
		authoritative_retirements = std::move(region.retirements);
	} else if (!authority_selected && cohort_keys.size() != 1U) {
		result["status"] = "STALE_APPLICATION";
		result["error"] = "GPU resident independent activation is not a singleton";
		return result;
	}
	std::size_t sink_activated = 0;
	for (std::size_t index = 0; index < inventories.size(); ++index) {
		if (!activation_required[index]) continue;
		const ParsedGpuChunkInventory &inventory = inventories[index];
		if (!render_sink_->set_gpu_resident_replacement(
				inventory.identity.key,
				inventory.identity.generation,
				inventory.identity.transition_mask,
				true
			)) {
			for (std::size_t rollback = 0, activated_count = 0;
					rollback < inventories.size() && activated_count < sink_activated;
					++rollback) {
				if (!activation_required[rollback]) continue;
				const ParsedGpuChunkInventory &activated = inventories[rollback];
				render_sink_->set_gpu_resident_replacement(
					activated.identity.key,
					activated.identity.generation,
					activated.identity.transition_mask,
					false
				);
				++activated_count;
			}
			result["status"] = "STALE_APPLICATION";
			result["error"] = "GPU resident cohort sink activation failed";
			return result;
		}
		++sink_activated;
	}
	for (std::size_t index = 0; index < inventories.size(); ++index) {
		if (!activation_required[index]) continue;
		const ParsedGpuChunkInventory &inventory = inventories[index];
		const WtApplicationStatus status =
			application_->confirm_external_visual_activation(
				inventory.identity.key, inventory.identity.generation
			);
		if (status != WtApplicationStatus::Ok &&
			status != WtApplicationStatus::AlreadyCurrent) {
			result["status"] = "STALE_APPLICATION";
			result["error"] = "GPU resident cohort readiness commit failed";
			return result;
		}
	}
	++gpu_resident_render_activation_cohorts_;
	gpu_resident_render_activation_cohort_chunks_ += inventories.size();
	gpu_resident_render_activated_chunks_ += sink_activated;
	flush_ready_independent_publication_regions();
	result["status"] = "ACTIVE";
	result["active"] = true;
	result["chunks"] = activated_chunks;
	result["retirements"] = gpu_cohort_keys(authoritative_retirements);
	result["chunk_count"] = static_cast<std::int64_t>(inventories.size());
	result["activated_chunk_count"] = static_cast<std::int64_t>(sink_activated);
	result["retained_active_chunk_count"] = static_cast<std::int64_t>(
		inventories.size() - sink_activated
	);
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
		)) {
		result["status"] = "STALE";
		result["error"] = "GPU resident chunk revision became stale";
		return result;
	}
	WtChunkApplicationRecord record;
	if (!application_ || !application_->copy_record(chunk_identity.key, record) ||
		record.generation != chunk_identity.generation ||
		record.visual_generation != chunk_identity.generation ||
		!record.visual_required ||
		(!record.visual_ready &&
			!record.external_visual_activation_required) ||
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
	const WtApplicationStatus activation_status =
		application_->confirm_external_visual_activation(
			chunk_identity.key, chunk_identity.generation
		);
	if (activation_status != WtApplicationStatus::Ok &&
		activation_status != WtApplicationStatus::AlreadyCurrent) {
		render_sink_->set_gpu_resident_replacement(
			chunk_identity.key,
			chunk_identity.generation,
			chunk_identity.transition_mask,
			false
		);
		result["status"] = "STALE_APPLICATION";
		result["error"] =
			"GPU resident activation did not commit application readiness";
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
	const bool coverage_staging_blocked =
		open_viewer_plan_publications_ != 0U ||
		!pending_chunk_replacements_.empty() ||
		!pending_chunk_retirements_.empty() ||
		!pending_render_retirements_.empty();
	result["coverage_staging_blocked"] = coverage_staging_blocked;
	result["pending_chunk_replacements"] = static_cast<std::int64_t>(
		pending_chunk_replacements_.size()
	);
	result["pending_chunk_retirements"] = static_cast<std::int64_t>(
		pending_chunk_retirements_.size()
	);
	result["pending_render_retirements"] = static_cast<std::int64_t>(
		pending_render_retirements_.size()
	);
	result["open_viewer_plan_publications"] = static_cast<std::int64_t>(
		open_viewer_plan_publications_
	);
	if (coverage_staging_blocked) {
		result["retire"] = retire;
		result["retire_count"] = 0;
		result["checked_count"] = terrain_identity_dictionaries.size();
		return result;
	}
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
			record.visual_required &&
			(record.visual_ready ||
				record.external_visual_activation_required);
		if (valid && !render_sink_->gpu_resident_replacement_matches(
				identity.key, identity.generation, identity.transition_mask
			)) {
			valid = render_sink_->set_gpu_resident_replacement(
				identity.key, identity.generation, identity.transition_mask, true
			);
		}
		if (valid) {
			const WtApplicationStatus activation_status =
				application_->confirm_external_visual_activation(
					identity.key, identity.generation
				);
			valid = activation_status == WtApplicationStatus::Ok ||
				activation_status == WtApplicationStatus::AlreadyCurrent;
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
	result["coverage_staging_blocked"] =
		open_viewer_plan_publications_ != 0U ||
		!pending_chunk_replacements_.empty() ||
		!pending_chunk_retirements_.empty() ||
		!pending_render_retirements_.empty();
	result["pending_chunk_replacements"] = static_cast<std::int64_t>(
		pending_chunk_replacements_.size()
	);
	result["pending_chunk_retirements"] = static_cast<std::int64_t>(
		pending_chunk_retirements_.size()
	);
	result["pending_render_retirements"] = static_cast<std::int64_t>(
		pending_render_retirements_.size()
	);
	result["open_viewer_plan_publications"] = static_cast<std::int64_t>(
		open_viewer_plan_publications_
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
	result["reserved_capture_failures"] = static_cast<std::int64_t>(
		queue_metrics.reserved_capture_failures
	);
	result["released_capture_slots"] = static_cast<std::int64_t>(
		queue_metrics.released_capture_slots
	);
	result["pre_mesh_field_captures"] = static_cast<std::int64_t>(
		queue_metrics.pre_mesh_field_captures
	);
	result["cpu_visual_mesh_omitted_captures"] = static_cast<std::int64_t>(
		queue_metrics.cpu_visual_mesh_omitted_captures
	);
	result["priority_dequeues"] = static_cast<std::int64_t>(
		queue_metrics.priority_dequeues
	);
	result["dequeue_superseded_requests"] = static_cast<std::int64_t>(
		queue_metrics.dequeue_superseded_requests
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
	result["prepared_chunks"] = static_cast<std::int64_t>(
		gpu_resident_render_prepared_chunks_
	);
	result["activation_cohorts"] = static_cast<std::int64_t>(
		gpu_resident_render_activation_cohorts_
	);
	result["activation_cohort_chunks"] = static_cast<std::int64_t>(
		gpu_resident_render_activation_cohort_chunks_
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
	result["cpu_topology_input_dependency"] = false;
	result["cpu_field_sampling"] = false;
	result["gpu_density_field_generation"] = true;
	result["gpu_material_field_generation"] = true;
	result["gpu_page_lattice_input"] = true;
	result["gpu_transvoxel_extraction"] = true;
	return result;
}

} // namespace world_transvoxel
