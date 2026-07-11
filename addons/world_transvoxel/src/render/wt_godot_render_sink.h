#pragma once

#include "render/wt_render_apply_queue.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstdint>
#include <map>
#include <thread>
#include <vector>

namespace godot {
class MeshInstance3D;
}

namespace world_transvoxel {

class WtGodotRenderSink final : public WtRenderSink {
public:
	explicit WtGodotRenderSink(godot::Node3D &owner) noexcept;

	bool apply_render(const WtRenderPayload &payload) override;
	bool remove_render(const WtChunkKey &key);
	bool begin_render_retirement(const WtChunkKey &key);
	void advance_retirements();
	void clear();
	std::size_t resource_count() const noexcept;
	std::size_t fading_count() const noexcept;
	std::size_t staged_count() const noexcept;
	void set_new_record_visibility_staging_enabled(bool enabled) noexcept;
	bool has_staged_records() const noexcept;
	void publish_staged_records() noexcept;
	WtGenerationToken applied_generation(const WtChunkKey &key) const noexcept;
	void set_shader_fade_parameter_enabled(bool enabled) noexcept;
	bool is_shader_fade_parameter_enabled() const noexcept;
	void set_transition_frames(std::uint32_t frames) noexcept;
	std::uint32_t get_transition_frames() const noexcept;
	void set_material_override(const godot::Variant &material);
	godot::Variant get_material_override() const;

private:
	struct Record {
		WtChunkKey key;
		godot::MeshInstance3D *instance = nullptr;
		WtGenerationToken generation;
		float current_transparency = 0.0F;
		float retirement_start_transparency = 0.0F;
		std::uint32_t introduction_frame = 0;
		std::uint32_t retirement_frame = 0;
		bool shader_fade_parameter_active = false;
		bool introducing = false;
		bool retiring = false;
		bool staged = false;
	};

	bool on_owner_thread() const noexcept;
	void set_record_transparency(Record &record, float value) noexcept;
	void apply_record_material_override(Record &record);
	godot::Node3D &owner_;
	std::thread::id owner_thread_;
	std::map<WtChunkKey, Record> records_;
	std::vector<Record> replacement_retirements_;
	godot::Variant material_override_;
	bool shader_fade_parameter_enabled_ = false;
	bool new_record_visibility_staging_enabled_ = false;
	std::uint32_t transition_frames_ = 0;
};

} // namespace world_transvoxel
