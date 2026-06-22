#include "physics/wt_godot_collision_sink.h"

#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace world_transvoxel {
namespace {

godot::String chunk_name(const WtChunkKey &key) {
	return godot::String("WT_Collision_") + godot::String::num_int64(key.x) + "_" +
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

} // namespace

WtGodotCollisionSink::WtGodotCollisionSink(godot::Node3D &owner) noexcept :
		owner_(owner), owner_thread_(std::this_thread::get_id()) {
}

bool WtGodotCollisionSink::apply_collision(const WtCollisionPayload &payload) {
	if (!on_owner_thread()) return false;
	if (payload.faces.empty()) {
		remove_collision(payload.key);
		return true;
	}
	godot::PackedVector3Array faces;
	faces.resize(static_cast<std::int64_t>(payload.faces.size()));
	for (std::size_t triangle = 0; triangle < payload.faces.size(); triangle += 3) {
		// Match Godot's clockwise front-face convention so the default
		// one-sided concave collision accepts rays and bodies from outside.
		faces.set(
			static_cast<std::int64_t>(triangle),
			to_godot(payload.faces[triangle])
		);
		faces.set(
			static_cast<std::int64_t>(triangle + 1),
			to_godot(payload.faces[triangle + 2])
		);
		faces.set(
			static_cast<std::int64_t>(triangle + 2),
			to_godot(payload.faces[triangle + 1])
		);
	}
	godot::Ref<godot::ConcavePolygonShape3D> shape;
	shape.instantiate();
	shape->set_faces(faces);

	Record &record = records_[payload.key];
	if (record.body == nullptr) {
		record.body = memnew(godot::StaticBody3D);
		record.shape = memnew(godot::CollisionShape3D);
		record.body->set_name(chunk_name(payload.key));
		record.shape->set_name("Shape");
		owner_.add_child(record.body);
		record.body->add_child(record.shape);
	}
	record.body->set_position(to_godot(payload.world_origin));
	record.shape->set_shape(shape);
	record.generation = payload.generation;
	return true;
}

bool WtGodotCollisionSink::remove_collision(const WtChunkKey &key) {
	if (!on_owner_thread()) {
		return false;
	}
	const auto iterator = records_.find(key);
	if (iterator == records_.end()) {
		return false;
	}
	owner_.remove_child(iterator->second.body);
	iterator->second.body->queue_free();
	records_.erase(iterator);
	return true;
}

void WtGodotCollisionSink::clear() {
	if (!on_owner_thread()) {
		return;
	}
	for (auto &entry : records_) {
		owner_.remove_child(entry.second.body);
		entry.second.body->queue_free();
	}
	records_.clear();
}

std::size_t WtGodotCollisionSink::resource_count() const noexcept {
	return records_.size();
}

WtGenerationToken WtGodotCollisionSink::applied_generation(
	const WtChunkKey &key
) const noexcept {
	const auto iterator = records_.find(key);
	return iterator == records_.end() ? WtGenerationToken{} : iterator->second.generation;
}

bool WtGodotCollisionSink::on_owner_thread() const noexcept {
	return std::this_thread::get_id() == owner_thread_;
}

} // namespace world_transvoxel
