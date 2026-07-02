#pragma once

#include "services/wt_runtime_config.h"

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

namespace world_transvoxel {

class WorldTransvoxelConfig : public godot::Resource {
	GDCLASS(WorldTransvoxelConfig, godot::Resource)

protected:
	static void _bind_methods();

public:
	std::int64_t get_schema_version() const noexcept;
	bool is_valid() const noexcept;
	godot::String get_validation_error() const;
	WtRuntimeConfig to_native() const noexcept;

	void set_active_chunk_capacity(std::int64_t value);
	std::int64_t get_active_chunk_capacity() const noexcept;
	void set_viewer_capacity(std::int64_t value);
	std::int64_t get_viewer_capacity() const noexcept;
	void set_demand_capacity_per_viewer(std::int64_t value);
	std::int64_t get_demand_capacity_per_viewer() const noexcept;
	void set_lod_refinement_radius_chunks(std::int64_t value);
	std::int64_t get_lod_refinement_radius_chunks() const noexcept;
	void set_storage_request_capacity(std::int64_t value);
	std::int64_t get_storage_request_capacity() const noexcept;
	void set_storage_completion_capacity(std::int64_t value);
	std::int64_t get_storage_completion_capacity() const noexcept;
	void set_encoded_page_entry_capacity(std::int64_t value);
	std::int64_t get_encoded_page_entry_capacity() const noexcept;
	void set_encoded_page_byte_capacity(std::int64_t value);
	std::int64_t get_encoded_page_byte_capacity() const noexcept;
	void set_decoded_page_entry_capacity(std::int64_t value);
	std::int64_t get_decoded_page_entry_capacity() const noexcept;
	void set_decoded_page_byte_capacity(std::int64_t value);
	std::int64_t get_decoded_page_byte_capacity() const noexcept;
	void set_mesh_entry_capacity(std::int64_t value);
	std::int64_t get_mesh_entry_capacity() const noexcept;
	void set_mesh_byte_capacity(std::int64_t value);
	std::int64_t get_mesh_byte_capacity() const noexcept;
	void set_render_entry_capacity(std::int64_t value);
	std::int64_t get_render_entry_capacity() const noexcept;
	void set_render_byte_capacity(std::int64_t value);
	std::int64_t get_render_byte_capacity() const noexcept;
	void set_collision_entry_capacity(std::int64_t value);
	std::int64_t get_collision_entry_capacity() const noexcept;
	void set_collision_byte_capacity(std::int64_t value);
	std::int64_t get_collision_byte_capacity() const noexcept;
	void set_trace_event_capacity(std::int64_t value);
	std::int64_t get_trace_event_capacity() const noexcept;
	void set_render_apply_budget(std::int64_t value);
	std::int64_t get_render_apply_budget() const noexcept;
	void set_collision_apply_budget(std::int64_t value);
	std::int64_t get_collision_apply_budget() const noexcept;
	void set_collision_activation_distance(double value);
	double get_collision_activation_distance() const noexcept;
	void set_collision_deactivation_distance(double value);
	double get_collision_deactivation_distance() const noexcept;
	void set_render_transition_frames(std::int64_t value);
	std::int64_t get_render_transition_frames() const noexcept;
	void set_shader_fade_parameter_enabled(bool value);
	bool is_shader_fade_parameter_enabled() const noexcept;

private:
	std::int64_t active_chunk_capacity_ = 256;
	std::int64_t viewer_capacity_ = 8;
	std::int64_t demand_capacity_per_viewer_ = 4096;
	std::int64_t lod_refinement_radius_chunks_ = 0;
	std::int64_t storage_request_capacity_ = 256;
	std::int64_t storage_completion_capacity_ = 256;
	std::int64_t encoded_page_entry_capacity_ = 256;
	std::int64_t encoded_page_byte_capacity_ = 64 * 1024 * 1024;
	std::int64_t decoded_page_entry_capacity_ = 128;
	std::int64_t decoded_page_byte_capacity_ = 64 * 1024 * 1024;
	std::int64_t mesh_entry_capacity_ = 128;
	std::int64_t mesh_byte_capacity_ = 128 * 1024 * 1024;
	std::int64_t render_entry_capacity_ = 128;
	std::int64_t render_byte_capacity_ = 128 * 1024 * 1024;
	std::int64_t collision_entry_capacity_ = 64;
	std::int64_t collision_byte_capacity_ = 64 * 1024 * 1024;
	std::int64_t trace_event_capacity_ = 65536;
	std::int64_t render_apply_budget_ = 4;
	std::int64_t collision_apply_budget_ = 2;
	double collision_activation_distance_ = 96.0;
	double collision_deactivation_distance_ = 128.0;
	std::int64_t render_transition_frames_ = 0;
	bool shader_fade_parameter_enabled_ = false;
};

} // namespace world_transvoxel
