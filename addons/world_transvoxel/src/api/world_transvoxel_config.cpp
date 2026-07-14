#include "api/world_transvoxel_config.h"

#include <godot_cpp/core/class_db.hpp>

namespace world_transvoxel {

#define WT_BIND_INT_PROPERTY(name, range) \
	godot::ClassDB::bind_method( \
		godot::D_METHOD("set_" #name, "value"), \
		&WorldTransvoxelConfig::set_##name \
	); \
	godot::ClassDB::bind_method( \
		godot::D_METHOD("get_" #name), \
		&WorldTransvoxelConfig::get_##name \
	); \
	ADD_PROPERTY( \
		godot::PropertyInfo( \
			godot::Variant::INT, #name, godot::PROPERTY_HINT_RANGE, range \
		), \
		"set_" #name, \
		"get_" #name \
	)

#define WT_BIND_FLOAT_PROPERTY(name, range) \
	godot::ClassDB::bind_method( \
		godot::D_METHOD("set_" #name, "value"), \
		&WorldTransvoxelConfig::set_##name \
	); \
	godot::ClassDB::bind_method( \
		godot::D_METHOD("get_" #name), \
		&WorldTransvoxelConfig::get_##name \
	); \
	ADD_PROPERTY( \
		godot::PropertyInfo( \
			godot::Variant::FLOAT, #name, godot::PROPERTY_HINT_RANGE, range \
		), \
		"set_" #name, \
		"get_" #name \
	)

void WorldTransvoxelConfig::_bind_methods() {
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_schema_version"),
		&WorldTransvoxelConfig::get_schema_version
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("is_valid"),
		&WorldTransvoxelConfig::is_valid
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_validation_error"),
		&WorldTransvoxelConfig::get_validation_error
	);

	WT_BIND_INT_PROPERTY(active_chunk_capacity, "1,65536,1");
	WT_BIND_INT_PROPERTY(viewer_capacity, "1,1024,1");
	WT_BIND_INT_PROPERTY(demand_capacity_per_viewer, "1,65536,1");
	WT_BIND_INT_PROPERTY(lod_refinement_radius_chunks, "0,65536,1");
	WT_BIND_INT_PROPERTY(storage_request_capacity, "1,65536,1");
	WT_BIND_INT_PROPERTY(storage_completion_capacity, "1,65536,1");
	WT_BIND_INT_PROPERTY(encoded_page_entry_capacity, "1,65536,1");
	WT_BIND_INT_PROPERTY(encoded_page_byte_capacity, "1,1073741824,1");
	WT_BIND_INT_PROPERTY(decoded_page_entry_capacity, "1,65536,1");
	WT_BIND_INT_PROPERTY(decoded_page_byte_capacity, "1,1073741824,1");
	WT_BIND_INT_PROPERTY(mesh_entry_capacity, "1,65536,1");
	WT_BIND_INT_PROPERTY(mesh_byte_capacity, "1,1073741824,1");
	WT_BIND_INT_PROPERTY(render_entry_capacity, "1,65536,1");
	WT_BIND_INT_PROPERTY(render_byte_capacity, "1,1073741824,1");
	WT_BIND_INT_PROPERTY(collision_entry_capacity, "1,65536,1");
	WT_BIND_INT_PROPERTY(collision_byte_capacity, "1,1073741824,1");
	WT_BIND_INT_PROPERTY(trace_event_capacity, "1,262144,1");
	WT_BIND_INT_PROPERTY(render_apply_budget, "0,128,1");
	WT_BIND_INT_PROPERTY(collision_apply_budget, "0,128,1");
	WT_BIND_INT_PROPERTY(render_transition_frames, "0,240,1");
	WT_BIND_FLOAT_PROPERTY(
		collision_activation_distance,
		"0,1000000,0.01,or_greater"
	);
	WT_BIND_FLOAT_PROPERTY(
		collision_deactivation_distance,
		"0,1000000,0.01,or_greater"
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("set_shader_fade_parameter_enabled", "enabled"),
		&WorldTransvoxelConfig::set_shader_fade_parameter_enabled
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("is_shader_fade_parameter_enabled"),
		&WorldTransvoxelConfig::is_shader_fade_parameter_enabled
	);
	ADD_PROPERTY(
		godot::PropertyInfo(
			godot::Variant::BOOL, "shader_fade_parameter_enabled"
		),
		"set_shader_fade_parameter_enabled",
		"is_shader_fade_parameter_enabled"
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("set_global_coarse_lod_coverage", "enabled"),
		&WorldTransvoxelConfig::set_global_coarse_lod_coverage
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("is_global_coarse_lod_coverage"),
		&WorldTransvoxelConfig::is_global_coarse_lod_coverage
	);
	ADD_PROPERTY(
		godot::PropertyInfo(
			godot::Variant::BOOL, "global_coarse_lod_coverage"
		),
		"set_global_coarse_lod_coverage",
		"is_global_coarse_lod_coverage"
	);
}

#undef WT_BIND_FLOAT_PROPERTY
#undef WT_BIND_INT_PROPERTY

std::int64_t WorldTransvoxelConfig::get_schema_version() const noexcept {
	return kWtRuntimeConfigSchema;
}

bool WorldTransvoxelConfig::is_valid() const noexcept {
	return wt_validate_runtime_config(to_native()) == WtRuntimeConfigStatus::Ok;
}

godot::String WorldTransvoxelConfig::get_validation_error() const {
	return wt_runtime_config_status_message(
		wt_validate_runtime_config(to_native())
	);
}

WtRuntimeConfig WorldTransvoxelConfig::to_native() const noexcept {
	WtRuntimeConfig result;
	result.active_chunk_capacity = active_chunk_capacity_;
	result.viewer_capacity = viewer_capacity_;
	result.demand_capacity_per_viewer = demand_capacity_per_viewer_;
	result.lod_refinement_radius_chunks = lod_refinement_radius_chunks_;
	result.storage_request_capacity = storage_request_capacity_;
	result.storage_completion_capacity = storage_completion_capacity_;
	result.encoded_page_entry_capacity = encoded_page_entry_capacity_;
	result.encoded_page_byte_capacity = encoded_page_byte_capacity_;
	result.decoded_page_entry_capacity = decoded_page_entry_capacity_;
	result.decoded_page_byte_capacity = decoded_page_byte_capacity_;
	result.mesh_entry_capacity = mesh_entry_capacity_;
	result.mesh_byte_capacity = mesh_byte_capacity_;
	result.render_entry_capacity = render_entry_capacity_;
	result.render_byte_capacity = render_byte_capacity_;
	result.collision_entry_capacity = collision_entry_capacity_;
	result.collision_byte_capacity = collision_byte_capacity_;
	result.trace_event_capacity = trace_event_capacity_;
	result.render_apply_budget = render_apply_budget_;
	result.collision_apply_budget = collision_apply_budget_;
	result.collision_activation_distance = collision_activation_distance_;
	result.collision_deactivation_distance = collision_deactivation_distance_;
	result.global_coarse_lod_coverage = global_coarse_lod_coverage_;
	return result;
}

#define WT_CONFIG_INT_ACCESSORS(name) \
	void WorldTransvoxelConfig::set_##name(std::int64_t value) { \
		if (name##_ == value) return; \
		name##_ = value; \
		emit_changed(); \
	} \
	std::int64_t WorldTransvoxelConfig::get_##name() const noexcept { \
		return name##_; \
	}

WT_CONFIG_INT_ACCESSORS(active_chunk_capacity)
WT_CONFIG_INT_ACCESSORS(viewer_capacity)
WT_CONFIG_INT_ACCESSORS(demand_capacity_per_viewer)
WT_CONFIG_INT_ACCESSORS(lod_refinement_radius_chunks)
WT_CONFIG_INT_ACCESSORS(storage_request_capacity)
WT_CONFIG_INT_ACCESSORS(storage_completion_capacity)
WT_CONFIG_INT_ACCESSORS(encoded_page_entry_capacity)
WT_CONFIG_INT_ACCESSORS(encoded_page_byte_capacity)
WT_CONFIG_INT_ACCESSORS(decoded_page_entry_capacity)
WT_CONFIG_INT_ACCESSORS(decoded_page_byte_capacity)
WT_CONFIG_INT_ACCESSORS(mesh_entry_capacity)
WT_CONFIG_INT_ACCESSORS(mesh_byte_capacity)
WT_CONFIG_INT_ACCESSORS(render_entry_capacity)
WT_CONFIG_INT_ACCESSORS(render_byte_capacity)
WT_CONFIG_INT_ACCESSORS(collision_entry_capacity)
WT_CONFIG_INT_ACCESSORS(collision_byte_capacity)
WT_CONFIG_INT_ACCESSORS(trace_event_capacity)
WT_CONFIG_INT_ACCESSORS(render_apply_budget)
WT_CONFIG_INT_ACCESSORS(collision_apply_budget)

#undef WT_CONFIG_INT_ACCESSORS

void WorldTransvoxelConfig::set_render_transition_frames(std::int64_t value) {
	if (value < 0) {
		value = 0;
	}
	if (value > 240) {
		value = 240;
	}
	if (render_transition_frames_ == value) return;
	render_transition_frames_ = value;
	emit_changed();
}

std::int64_t WorldTransvoxelConfig::get_render_transition_frames() const noexcept {
	return render_transition_frames_;
}

void WorldTransvoxelConfig::set_collision_activation_distance(double value) {
	if (collision_activation_distance_ == value) return;
	collision_activation_distance_ = value;
	emit_changed();
}

double WorldTransvoxelConfig::get_collision_activation_distance() const noexcept {
	return collision_activation_distance_;
}

void WorldTransvoxelConfig::set_collision_deactivation_distance(double value) {
	if (collision_deactivation_distance_ == value) return;
	collision_deactivation_distance_ = value;
	emit_changed();
}

double WorldTransvoxelConfig::get_collision_deactivation_distance() const noexcept {
	return collision_deactivation_distance_;
}

void WorldTransvoxelConfig::set_shader_fade_parameter_enabled(bool value) {
	if (shader_fade_parameter_enabled_ == value) return;
	shader_fade_parameter_enabled_ = value;
	emit_changed();
}

bool WorldTransvoxelConfig::is_shader_fade_parameter_enabled() const noexcept {
	return shader_fade_parameter_enabled_;
}

void WorldTransvoxelConfig::set_global_coarse_lod_coverage(bool value) {
	if (global_coarse_lod_coverage_ == value) return;
	global_coarse_lod_coverage_ = value;
	emit_changed();
}

bool WorldTransvoxelConfig::is_global_coarse_lod_coverage() const noexcept {
	return global_coarse_lod_coverage_;
}

} // namespace world_transvoxel
