#include "render/wt_godot_render_sink.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <vector>

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

godot::Color surface_material_blend_weights(std::uint16_t material) {
	switch (material) {
		case 2:
			return { 1.0F, 0.0F, 0.0F, 0.0F };
		case 3:
			return { 0.0F, 1.0F, 0.0F, 0.0F };
		case 4:
			return { 0.0F, 0.0F, 1.0F, 0.0F };
		case 5:
			return { 0.0F, 0.0F, 0.0F, 1.0F };
		default:
			return { 0.0F, 0.0F, 0.0F, 0.0F };
	}
}

bool add_render_surface(
	godot::ArrayMesh &mesh,
	const std::vector<WtRenderVertex> &vertices,
	const std::vector<std::uint32_t> &source_indices,
	const WtGridPoint &origin,
	const godot::String &name
) {
	if (source_indices.empty()) {
		return vertices.empty();
	}
	if ((source_indices.size() % 3U) != 0) {
		return false;
	}
	godot::PackedVector3Array positions;
	godot::PackedVector3Array normals;
	godot::PackedVector2Array materials;
	godot::PackedColorArray surface_material_blends;
	godot::PackedInt32Array indices;
	positions.resize(static_cast<std::int64_t>(vertices.size()));
	normals.resize(static_cast<std::int64_t>(vertices.size()));
	materials.resize(static_cast<std::int64_t>(vertices.size()));
	surface_material_blends.resize(static_cast<std::int64_t>(vertices.size()));
	const godot::Vector3 world_origin = to_godot(origin);
	for (std::size_t index = 0; index < vertices.size(); ++index) {
		const WtRenderVertex &vertex = vertices[index];
		positions.set(
			static_cast<std::int64_t>(index),
			to_godot(vertex.position) + world_origin
		);
		normals.set(static_cast<std::int64_t>(index), to_godot(vertex.normal));
		materials.set(static_cast<std::int64_t>(index), {
			static_cast<godot::real_t>(vertex.material),
			static_cast<godot::real_t>(
				vertex.material_authored ? 1.0F : 0.0F
			)
		});
		surface_material_blends.set(
			static_cast<std::int64_t>(index),
			surface_material_blend_weights(vertex.material)
		);
	}
	std::vector<std::int32_t> godot_indices;
	godot_indices.reserve(source_indices.size());
	for (std::size_t triangle = 0; triangle < source_indices.size(); triangle += 3) {
		const std::uint32_t a = source_indices[triangle];
		const std::uint32_t b = source_indices[triangle + 1];
		const std::uint32_t c = source_indices[triangle + 2];
		if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) {
			return false;
		}
		godot_indices.push_back(static_cast<std::int32_t>(a));
		godot_indices.push_back(static_cast<std::int32_t>(c));
		godot_indices.push_back(static_cast<std::int32_t>(b));
	}
	indices.resize(static_cast<std::int64_t>(godot_indices.size()));
	for (std::size_t index = 0; index < godot_indices.size(); ++index) {
		indices.set(static_cast<std::int64_t>(index), godot_indices[index]);
	}
	godot::Array arrays;
	arrays.resize(godot::Mesh::ARRAY_MAX);
	arrays[godot::Mesh::ARRAY_VERTEX] = positions;
	arrays[godot::Mesh::ARRAY_NORMAL] = normals;
	arrays[godot::Mesh::ARRAY_COLOR] = surface_material_blends;
	arrays[godot::Mesh::ARRAY_TEX_UV2] = materials;
	arrays[godot::Mesh::ARRAY_INDEX] = indices;
	mesh.add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh.surface_set_name(mesh.get_surface_count() - 1, name);
	return true;
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
	record.instance->set(godot::StringName("material_override"), godot::Variant());
	const godot::Ref<godot::Mesh> mesh = record.instance->get_mesh();
	const godot::ArrayMesh *array_mesh = mesh.is_valid() ?
		godot::Object::cast_to<godot::ArrayMesh>(mesh.ptr()) : nullptr;
	if (array_mesh == nullptr) {
		return;
	}
	for (std::int32_t surface = 0; surface < array_mesh->get_surface_count(); ++surface) {
		const bool water = array_mesh->surface_get_name(surface) == godot::String("water");
		const godot::Variant &material = water &&
			water_material_override_.get_type() != godot::Variant::NIL ?
			water_material_override_ : material_override_;
		record.instance->set(
			godot::StringName(
				godot::String("surface_material_override/") +
				godot::String::num_int64(surface)
			),
			material
		);
	}
}

bool WtGodotRenderSink::apply_render(const WtRenderPayload &payload) {
	if (!on_owner_thread()) return false;
	if (payload.indices.empty() && payload.water_indices.empty()) {
		const auto iterator = records_.find(payload.key);
		if (iterator != records_.end() &&
				should_stage_existing_replacement(payload.key)) {
			Record &record = iterator->second;
			record.staged_mesh.unref();
			record.staged_generation = payload.generation;
			record.staged = true;
			record.staged_empty = true;
			record.retiring = false;
			record.introducing = false;
			record.retirement_frame = 0;
			record.introduction_frame = 0;
			return true;
		}
		remove_render(payload.key);
		return true;
	}
	// Render chunks share seam vertices across separate MeshInstance3D draw
	// calls. Store render positions in a common world-space frame and keep the
	// instance transform identity so the GPU receives identical seam positions
	// instead of recomputing equivalent world positions from different chunk
	// origins.
	godot::Ref<godot::ArrayMesh> mesh;
	mesh.instantiate();
	if (!add_render_surface(
			**mesh,
			payload.vertices,
			payload.indices,
			payload.world_origin,
			"terrain"
		) || !add_render_surface(
			**mesh,
			payload.water_vertices,
			payload.water_indices,
			payload.world_origin,
			"water"
		)) {
		return false;
	}

	Record &record = records_[payload.key];
	const bool created = record.instance == nullptr;
	if (created) {
		record.instance = memnew(godot::MeshInstance3D);
		record.key = payload.key;
		record.instance->set_name(chunk_name(payload.key));
		owner_.add_child(record.instance);
		record.staged = should_stage_created_record(payload.key);
	} else {
		if (should_stage_existing_replacement(payload.key)) {
			record.staged_mesh = mesh;
			record.staged_generation = payload.generation;
			record.staged = true;
			record.staged_empty = false;
			record.retiring = false;
			record.introducing = false;
			record.retirement_frame = 0;
			record.introduction_frame = 0;
			return true;
		}
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
	record.introducing = !created && transition_frames_ > 0U;
	record.introduction_frame = 0;
	record.staged_mesh.unref();
	record.staged_generation = {};
	record.staged_empty = false;
	record.instance->set_position(godot::Vector3{});
	record.instance->set_mesh(mesh);
	record.instance->set_visible(!record.staged);
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
	if (new_record_visibility_staging_enabled_) {
		record.staged = true;
		record.staged_empty = true;
		record.staged_mesh.unref();
		record.staged_generation = {};
		record.retiring = false;
		record.introducing = false;
		record.retirement_frame = 0;
		record.introduction_frame = 0;
		record.retirement_start_transparency = 0.0F;
		set_record_transparency(record, 0.0F);
		return true;
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

std::size_t WtGodotRenderSink::staged_count() const noexcept {
	std::size_t count = 0;
	for (const auto &entry : records_) {
		count += entry.second.staged ? 1U : 0U;
	}
	return count;
}

void WtGodotRenderSink::set_new_record_visibility_staging_enabled(
	bool enabled
) noexcept {
	new_record_visibility_staging_enabled_ = enabled;
}

void WtGodotRenderSink::set_visibility_staging_reference_chunks(
	const std::vector<WtChunkKey> &keys
) {
	visibility_staging_reference_chunks_ = keys;
}

bool WtGodotRenderSink::has_staged_records() const noexcept {
	for (const auto &entry : records_) {
		if (entry.second.staged) {
			return true;
		}
	}
	return false;
}

void WtGodotRenderSink::publish_staged_records() noexcept {
	if (!on_owner_thread()) {
		return;
	}
	for (auto &entry : records_) {
		Record &record = entry.second;
		if (!record.staged || record.instance == nullptr) {
			continue;
		}
		if (record.staged_empty) {
			continue;
		}
		if (record.staged_mesh.is_valid()) {
			record.instance->set_mesh(record.staged_mesh);
			apply_record_material_override(record);
			record.generation = record.staged_generation;
			record.staged_mesh.unref();
			record.staged_generation = {};
			record.staged_empty = false;
			record.retiring = false;
			record.introducing = false;
			record.retirement_frame = 0;
			record.introduction_frame = 0;
			record.retirement_start_transparency = 0.0F;
			set_record_transparency(record, 0.0F);
		}
		record.staged = false;
		record.instance->set_visible(true);
	}
	for (auto iterator = records_.begin(); iterator != records_.end();) {
		Record &record = iterator->second;
		if (!record.staged || !record.staged_empty || record.instance == nullptr) {
			++iterator;
			continue;
		}
		owner_.remove_child(record.instance);
		record.instance->queue_free();
		iterator = records_.erase(iterator);
	}
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

void WtGodotRenderSink::set_water_material_override(
	const godot::Variant &material
) {
	water_material_override_ = material;
	for (auto &entry : records_) {
		apply_record_material_override(entry.second);
	}
	for (Record &record : replacement_retirements_) {
		apply_record_material_override(record);
	}
}

godot::Variant WtGodotRenderSink::get_water_material_override() const {
	return water_material_override_;
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

bool WtGodotRenderSink::should_stage_created_record(
	const WtChunkKey &key
) const noexcept {
	if (!new_record_visibility_staging_enabled_) {
		return false;
	}
	if (visibility_staging_reference_chunks_.empty()) {
		return false;
	}
	const WtChunkBounds bounds = wt_chunk_bounds(key);
	bool touches_replacement_region = false;
	for (const WtChunkKey &reference_key : visibility_staging_reference_chunks_) {
		const WtChunkBounds reference_bounds = wt_chunk_bounds(reference_key);
		const bool overlaps =
			bounds.minimum.x < reference_bounds.maximum.x &&
			reference_bounds.minimum.x < bounds.maximum.x &&
			bounds.minimum.y < reference_bounds.maximum.y &&
			reference_bounds.minimum.y < bounds.maximum.y &&
			bounds.minimum.z < reference_bounds.maximum.z &&
			reference_bounds.minimum.z < bounds.maximum.z;
		const bool touches_or_overlaps =
			bounds.minimum.x <= reference_bounds.maximum.x &&
			reference_bounds.minimum.x <= bounds.maximum.x &&
			bounds.minimum.y <= reference_bounds.maximum.y &&
			reference_bounds.minimum.y <= bounds.maximum.y &&
			bounds.minimum.z <= reference_bounds.maximum.z &&
			reference_bounds.minimum.z <= bounds.maximum.z;
		if (overlaps || (touches_or_overlaps && key.lod != reference_key.lod)) {
			touches_replacement_region = true;
			break;
		}
	}
	if (!touches_replacement_region) {
		return false;
	}
	for (const auto &entry : records_) {
		const WtChunkKey &visible_key = entry.first;
		const Record &record = entry.second;
		if (visible_key == key || record.instance == nullptr ||
				!record.instance->is_visible()) {
			continue;
		}
		const WtChunkBounds visible_bounds = wt_chunk_bounds(visible_key);
		const bool visible_overlap =
			bounds.minimum.x < visible_bounds.maximum.x &&
			visible_bounds.minimum.x < bounds.maximum.x &&
			bounds.minimum.y < visible_bounds.maximum.y &&
			visible_bounds.minimum.y < bounds.maximum.y &&
			bounds.minimum.z < visible_bounds.maximum.z &&
			visible_bounds.minimum.z < bounds.maximum.z;
		if (visible_overlap) {
			return true;
		}
	}
	return false;
}

bool WtGodotRenderSink::should_stage_existing_replacement(
	const WtChunkKey &key
) const noexcept {
	(void)key;
	return new_record_visibility_staging_enabled_;
}

} // namespace world_transvoxel
