#include "render/wt_godot_render_sink.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace world_transvoxel {
namespace {

constexpr std::uint32_t kDefaultDisabledTransitionFrames = 0U;
constexpr const char *kFadeOpacityShaderParameter = "wt_fade_opacity";
constexpr float kDefaultFadeOpacity = 1.0F;
constexpr float kFadeOpacityEpsilon = 0.0001F;

godot::String chunk_name(const WtChunkKey &key) {
	return godot::String("WT_Render_") + godot::String::num_int64(key.x) + "_" +
		godot::String::num_int64(key.y) + "_" + godot::String::num_int64(key.z) +
		"_L" + godot::String::num_int64(key.lod);
}

godot::String retiring_chunk_name(
	const WtChunkKey &key,
	const WtGenerationToken &generation
) {
	return chunk_name(key) + "_retiring_" +
		godot::String::num_uint64(generation.value);
}

godot::Vector3 to_godot(const WtVec3 &value) {
	return { value.x, value.y, value.z };
}

godot::Vector3 to_godot(const WtGridPoint &value) {
	return {
		static_cast<godot::real_t>(value.x),
		static_cast<godot::real_t>(value.y),
		static_cast<godot::real_t>(value.z),
	};
}

float clamp_unit(float value) {
	if (value < 0.0F) {
		return 0.0F;
	}
	if (value > 1.0F) {
		return 1.0F;
	}
	return value;
}

} // namespace

WtGodotRenderSink::WtGodotRenderSink(godot::Node3D &owner) noexcept :
		owner_(owner), owner_thread_(std::this_thread::get_id()) {
}

void WtGodotRenderSink::set_record_transparency(
	Record &record,
	float value
) noexcept {
	record.current_transparency = clamp_unit(value);
	record.instance->set_transparency(record.current_transparency);
	if (!shader_fade_parameter_enabled_) {
		return;
	}
	const float fade_opacity = 1.0F - record.current_transparency;
	const godot::StringName parameter(kFadeOpacityShaderParameter);
	if (fade_opacity < (kDefaultFadeOpacity - kFadeOpacityEpsilon)) {
		record.instance->set_instance_shader_parameter(parameter, fade_opacity);
		record.shader_fade_parameter_active = true;
		return;
	}
	if (record.shader_fade_parameter_active) {
		record.instance->set_instance_shader_parameter(parameter, godot::Variant());
		record.shader_fade_parameter_active = false;
	}
}

void WtGodotRenderSink::apply_record_material_override(Record &record) {
	if (record.instance == nullptr) {
		return;
	}
	record.instance->set(godot::StringName("material_override"), material_override_);
}

bool WtGodotRenderSink::apply_render(const WtRenderPayload &payload) {
	if (!on_owner_thread()) return false;
	if (payload.indices.empty()) {
		remove_render(payload.key);
		return true;
	}
	godot::PackedVector3Array positions;
	godot::PackedVector3Array normals;
	godot::PackedVector2Array materials;
	godot::PackedInt32Array indices;
	positions.resize(static_cast<std::int64_t>(payload.vertices.size()));
	normals.resize(static_cast<std::int64_t>(payload.vertices.size()));
	materials.resize(static_cast<std::int64_t>(payload.vertices.size()));
	indices.resize(static_cast<std::int64_t>(payload.indices.size()));
	for (std::size_t index = 0; index < payload.vertices.size(); ++index) {
		const WtRenderVertex &vertex = payload.vertices[index];
		positions.set(static_cast<std::int64_t>(index), to_godot(vertex.position));
		normals.set(static_cast<std::int64_t>(index), to_godot(vertex.normal));
		materials.set(static_cast<std::int64_t>(index), {
			static_cast<godot::real_t>(vertex.material), 0.0
		});
	}
	for (std::size_t triangle = 0; triangle < payload.indices.size(); triangle += 3) {
		// World Transvoxel stores outward triangles counterclockwise. Godot
		// defines clockwise triangle winding as front-facing, so adapt only at
		// the engine boundary while preserving the authoritative payload.
		indices.set(
			static_cast<std::int64_t>(triangle),
			static_cast<std::int32_t>(payload.indices[triangle])
		);
		indices.set(
			static_cast<std::int64_t>(triangle + 1),
			static_cast<std::int32_t>(payload.indices[triangle + 2])
		);
		indices.set(
			static_cast<std::int64_t>(triangle + 2),
			static_cast<std::int32_t>(payload.indices[triangle + 1])
		);
	}
	godot::Array arrays;
	arrays.resize(godot::Mesh::ARRAY_MAX);
	arrays[godot::Mesh::ARRAY_VERTEX] = positions;
	arrays[godot::Mesh::ARRAY_NORMAL] = normals;
	arrays[godot::Mesh::ARRAY_TEX_UV2] = materials;
	arrays[godot::Mesh::ARRAY_INDEX] = indices;
	godot::Ref<godot::ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, arrays);

	Record &record = records_[payload.key];
	const bool created = record.instance == nullptr;
	if (created) {
		record.instance = memnew(godot::MeshInstance3D);
		record.key = payload.key;
		record.instance->set_name(chunk_name(payload.key));
		owner_.add_child(record.instance);
	} else {
		godot::Ref<godot::Mesh> retiring_mesh = record.instance->get_mesh();
		if (transition_frames_ > 0U && retiring_mesh.is_valid()) {
			Record retirement;
			retirement.instance = memnew(godot::MeshInstance3D);
			retirement.key = payload.key;
			retirement.generation = record.generation;
			retirement.instance->set_name(
				retiring_chunk_name(record.key, record.generation)
			);
			retirement.instance->set_position(record.instance->get_position());
			retirement.instance->set_mesh(retiring_mesh);
			retirement.retiring = true;
			retirement.retirement_frame = 0;
			retirement.retirement_start_transparency = record.current_transparency;
			owner_.add_child(retirement.instance);
			apply_record_material_override(retirement);
			set_record_transparency(retirement, record.current_transparency);
			replacement_retirements_.push_back(retirement);
		}
		record.key = payload.key;
		record.instance->set_name(chunk_name(payload.key));
	}
	record.retiring = false;
	record.retirement_frame = 0;
	record.retirement_start_transparency = 0.0F;
	record.introducing = transition_frames_ > 0U;
	record.introduction_frame = 0;
	record.instance->set_position(to_godot(payload.world_origin));
	record.instance->set_mesh(mesh);
	apply_record_material_override(record);
	set_record_transparency(record, record.introducing ? 1.0F : 0.0F);
	record.generation = payload.generation;
	return true;
}

bool WtGodotRenderSink::remove_render(const WtChunkKey &key) {
	if (!on_owner_thread()) {
		return false;
	}
	const auto iterator = records_.find(key);
	bool removed = false;
	if (iterator != records_.end()) {
		owner_.remove_child(iterator->second.instance);
		iterator->second.instance->queue_free();
		records_.erase(iterator);
		removed = true;
	}
	for (auto retirement = replacement_retirements_.begin();
			retirement != replacement_retirements_.end();) {
		if (retirement->key == key) {
			owner_.remove_child(retirement->instance);
			retirement->instance->queue_free();
			retirement = replacement_retirements_.erase(retirement);
			removed = true;
		} else {
			++retirement;
		}
	}
	return removed;
}

bool WtGodotRenderSink::begin_render_retirement(const WtChunkKey &key) {
	if (!on_owner_thread()) {
		return false;
	}
	const auto iterator = records_.find(key);
	if (iterator == records_.end()) {
		return false;
	}
	Record &record = iterator->second;
	if (record.instance == nullptr) {
		records_.erase(iterator);
		return false;
	}
	if (transition_frames_ == 0U) {
		owner_.remove_child(record.instance);
		record.instance->queue_free();
		records_.erase(iterator);
		return true;
	}
	record.retiring = true;
	record.introducing = false;
	record.retirement_frame = 0;
	record.retirement_start_transparency = record.current_transparency;
	return true;
}

void WtGodotRenderSink::advance_retirements() {
	if (!on_owner_thread()) {
		return;
	}
	for (auto iterator = replacement_retirements_.begin();
			iterator != replacement_retirements_.end();) {
		Record &record = *iterator;
		++record.retirement_frame;
		const float progress = static_cast<float>(record.retirement_frame) /
			static_cast<float>(transition_frames_);
		const float transparency = record.retirement_start_transparency +
			((1.0F - record.retirement_start_transparency) * progress);
		set_record_transparency(record, transparency);
		if (record.retirement_frame >= transition_frames_) {
			owner_.remove_child(record.instance);
			record.instance->queue_free();
			iterator = replacement_retirements_.erase(iterator);
		} else {
			++iterator;
		}
	}
	for (auto iterator = records_.begin(); iterator != records_.end();) {
		Record &record = iterator->second;
		if (record.retiring) {
			++record.retirement_frame;
			const float progress = static_cast<float>(record.retirement_frame) /
				static_cast<float>(transition_frames_);
			const float transparency = record.retirement_start_transparency +
				((1.0F - record.retirement_start_transparency) * progress);
			set_record_transparency(record, transparency);
			if (record.retirement_frame >= transition_frames_) {
				owner_.remove_child(record.instance);
				record.instance->queue_free();
				iterator = records_.erase(iterator);
			} else {
				++iterator;
			}
			continue;
		}
		if (record.introducing) {
			++record.introduction_frame;
			const float progress = static_cast<float>(record.introduction_frame) /
				static_cast<float>(transition_frames_);
			set_record_transparency(record, 1.0F - progress);
			if (record.introduction_frame >= transition_frames_) {
				record.introducing = false;
				record.introduction_frame = 0;
				set_record_transparency(record, 0.0F);
			}
			++iterator;
			continue;
		}
		++iterator;
	}
}

void WtGodotRenderSink::clear() {
	if (!on_owner_thread()) {
		return;
	}
	for (auto &entry : records_) {
		owner_.remove_child(entry.second.instance);
		entry.second.instance->queue_free();
	}
	records_.clear();
	for (auto &record : replacement_retirements_) {
		owner_.remove_child(record.instance);
		record.instance->queue_free();
	}
	replacement_retirements_.clear();
}

std::size_t WtGodotRenderSink::resource_count() const noexcept {
	return records_.size() + replacement_retirements_.size();
}

std::size_t WtGodotRenderSink::fading_count() const noexcept {
	std::size_t count = replacement_retirements_.size();
	for (const auto &entry : records_) {
		count += (entry.second.retiring || entry.second.introducing) ? 1U : 0U;
	}
	return count;
}

WtGenerationToken WtGodotRenderSink::applied_generation(
	const WtChunkKey &key
) const noexcept {
	const auto iterator = records_.find(key);
	return iterator == records_.end() ? WtGenerationToken{} : iterator->second.generation;
}

void WtGodotRenderSink::set_shader_fade_parameter_enabled(
	bool enabled
) noexcept {
	shader_fade_parameter_enabled_ = enabled;
}

bool WtGodotRenderSink::is_shader_fade_parameter_enabled() const noexcept {
	return shader_fade_parameter_enabled_;
}

void WtGodotRenderSink::set_material_override(
	const godot::Variant &material
) {
	material_override_ = material;
	for (auto &entry : records_) {
		apply_record_material_override(entry.second);
	}
	for (Record &record : replacement_retirements_) {
		apply_record_material_override(record);
	}
}

godot::Variant WtGodotRenderSink::get_material_override() const {
	return material_override_;
}

void WtGodotRenderSink::set_transition_frames(std::uint32_t frames) noexcept {
	transition_frames_ = frames;
	if (transition_frames_ == kDefaultDisabledTransitionFrames) {
		for (auto &entry : records_) {
			Record &record = entry.second;
			record.introducing = false;
			record.retiring = false;
			record.introduction_frame = 0;
			record.retirement_frame = 0;
			record.retirement_start_transparency = 0.0F;
			set_record_transparency(record, 0.0F);
		}
		for (auto &record : replacement_retirements_) {
			owner_.remove_child(record.instance);
			record.instance->queue_free();
		}
		replacement_retirements_.clear();
	}
}

std::uint32_t WtGodotRenderSink::get_transition_frames() const noexcept {
	return transition_frames_;
}

bool WtGodotRenderSink::on_owner_thread() const noexcept {
	return std::this_thread::get_id() == owner_thread_;
}

} // namespace world_transvoxel
