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
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace world_transvoxel {
namespace {

constexpr std::uint32_t kRenderRetirementFadeFrames = 12U;
constexpr std::uint32_t kRenderIntroductionFadeFrames = 12U;

godot::String chunk_name(const WtChunkKey &key) {
	return godot::String("WT_Render_") + godot::String::num_int64(key.x) + "_" +
		godot::String::num_int64(key.y) + "_" + godot::String::num_int64(key.z) +
		"_L" + godot::String::num_int64(key.lod);
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
		record.instance->set_name(chunk_name(payload.key));
		owner_.add_child(record.instance);
	}
	record.retiring = false;
	record.retirement_frame = 0;
	record.retirement_start_transparency = 0.0F;
	record.introducing = created;
	record.introduction_frame = 0;
	record.instance->set_position(to_godot(payload.world_origin));
	record.instance->set_mesh(mesh);
	set_record_transparency(record, created ? 1.0F : 0.0F);
	record.generation = payload.generation;
	return true;
}

bool WtGodotRenderSink::remove_render(const WtChunkKey &key) {
	if (!on_owner_thread()) {
		return false;
	}
	const auto iterator = records_.find(key);
	if (iterator == records_.end()) {
		return false;
	}
	owner_.remove_child(iterator->second.instance);
	iterator->second.instance->queue_free();
	records_.erase(iterator);
	return true;
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
	for (auto iterator = records_.begin(); iterator != records_.end();) {
		Record &record = iterator->second;
		if (record.retiring) {
			++record.retirement_frame;
			const float progress = static_cast<float>(record.retirement_frame) /
				static_cast<float>(kRenderRetirementFadeFrames);
			const float transparency = record.retirement_start_transparency +
				((1.0F - record.retirement_start_transparency) * progress);
			set_record_transparency(record, transparency);
			if (record.retirement_frame >= kRenderRetirementFadeFrames) {
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
				static_cast<float>(kRenderIntroductionFadeFrames);
			set_record_transparency(record, 1.0F - progress);
			if (record.introduction_frame >= kRenderIntroductionFadeFrames) {
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
}

std::size_t WtGodotRenderSink::resource_count() const noexcept {
	return records_.size();
}

std::size_t WtGodotRenderSink::fading_count() const noexcept {
	std::size_t count = 0;
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

bool WtGodotRenderSink::on_owner_thread() const noexcept {
	return std::this_thread::get_id() == owner_thread_;
}

} // namespace world_transvoxel
