#include "api/world_transvoxel_terrain.h"

#include "physics/wt_godot_collision_sink.h"
#include "render/wt_godot_render_sink.h"

#include <godot_cpp/classes/project_settings.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace world_transvoxel {
namespace {

std::filesystem::path globalized_path(const godot::String &path) {
	const godot::String global =
		godot::ProjectSettings::get_singleton()->globalize_path(path);
	const godot::CharString utf8 = global.utf8();
	return std::filesystem::u8path(utf8.get_data());
}

std::uint32_t compact_seed(std::int64_t seed) noexcept {
	const std::uint64_t magnitude = seed < 0 ?
		static_cast<std::uint64_t>(-(seed + 1)) + 1U :
		static_cast<std::uint64_t>(seed);
	return static_cast<std::uint32_t>(magnitude);
}

} // namespace

bool WorldTransvoxelTerrain::start_world(
	const godot::String &world_manifest_path,
	const godot::String &object_root
) {
	if (!is_configuration_valid()) {
		synchronous_world_error_ = get_configuration_error();
		return false;
	}
	if (lifecycle_ &&
		lifecycle_->state() != WtWorldLifecycleState::Stopped) {
		synchronous_world_error_ =
			"world lifecycle state does not allow startup";
		return false;
	}
	const WtRuntimeConfig config = configuration_->to_native();
	auto lifecycle = std::make_unique<WtWorldLifecycleService>(config);
	const WtWorldLifecycleStatus status = lifecycle->start(
		globalized_path(world_manifest_path),
		globalized_path(object_root)
	);
	if (status != WtWorldLifecycleStatus::Ok) {
		synchronous_world_error_ =
			wt_world_lifecycle_status_message(status);
		return false;
	}
	lifecycle_ = std::move(lifecycle);
	render_sink_->set_shader_fade_parameter_enabled(
		configuration_->is_shader_fade_parameter_enabled()
	);
	render_sink_->set_transition_frames(static_cast<std::uint32_t>(
		configuration_->get_render_transition_frames()
	));
	render_sink_->clear();
	collision_sink_->clear();
	reset_world_application(static_cast<std::size_t>(
		config.active_chunk_capacity
	));
	render_apply_budget_ = static_cast<std::size_t>(
		config.render_apply_budget
	);
	collision_apply_budget_ = static_cast<std::size_t>(
		config.collision_apply_budget
	);
	synchronous_world_error_ = "ok";
	emit_lifecycle_state(WtWorldLifecycleState::Starting);
	return true;
}

bool WorldTransvoxelTerrain::start_procedural_world(
	std::int64_t chunk_count_x,
	std::int64_t chunk_count_z,
	std::int64_t seed,
	std::int64_t source_revision,
	const godot::String &object_root
) {
	if (!is_configuration_valid()) {
		synchronous_world_error_ = get_configuration_error();
		return false;
	}
	if (lifecycle_ &&
		lifecycle_->state() != WtWorldLifecycleState::Stopped) {
		synchronous_world_error_ =
			"world lifecycle state does not allow startup";
		return false;
	}
	if (chunk_count_x <= 0 || chunk_count_z <= 0 ||
		chunk_count_x > 4096 || chunk_count_z > 4096 ||
		source_revision <= 0 || object_root.is_empty()) {
		synchronous_world_error_ =
			"procedural world descriptor is invalid";
		return false;
	}
	const std::uint64_t page_count =
		static_cast<std::uint64_t>(chunk_count_x) *
		static_cast<std::uint64_t>(chunk_count_z);
	if (page_count == 0 || page_count > 262144U) {
		synchronous_world_error_ =
			"procedural world page count exceeds compact runtime limit";
		return false;
	}
	const WtRuntimeConfig config = configuration_->to_native();
	auto lifecycle = std::make_unique<WtWorldLifecycleService>(config);
	WtProceduralWorldDescriptor descriptor;
	descriptor.chunk_count_x = static_cast<std::uint32_t>(chunk_count_x);
	descriptor.chunk_count_z = static_cast<std::uint32_t>(chunk_count_z);
	descriptor.chunk_y = 0;
	descriptor.source_revision = static_cast<std::uint64_t>(source_revision);
	descriptor.world_revision = 0;
	descriptor.seed = compact_seed(seed);
	const WtWorldLifecycleStatus status = lifecycle->start_procedural(
		descriptor,
		globalized_path(object_root)
	);
	if (status != WtWorldLifecycleStatus::Ok) {
		synchronous_world_error_ =
			wt_world_lifecycle_status_message(status);
		return false;
	}
	lifecycle_ = std::move(lifecycle);
	render_sink_->set_shader_fade_parameter_enabled(
		configuration_->is_shader_fade_parameter_enabled()
	);
	render_sink_->set_transition_frames(static_cast<std::uint32_t>(
		configuration_->get_render_transition_frames()
	));
	render_sink_->clear();
	collision_sink_->clear();
	reset_world_application(static_cast<std::size_t>(
		config.active_chunk_capacity
	));
	render_apply_budget_ = static_cast<std::size_t>(
		config.render_apply_budget
	);
	collision_apply_budget_ = static_cast<std::size_t>(
		config.collision_apply_budget
	);
	synchronous_world_error_ = "ok";
	emit_lifecycle_state(WtWorldLifecycleState::Starting);
	return true;
}

} // namespace world_transvoxel
