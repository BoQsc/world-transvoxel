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
	godot::PackedVector3Array faces;
	faces.resize(3);
	faces.set(0, godot::Vector3(0.0, 0.0, 0.0));
	faces.set(1, godot::Vector3(1.0, 0.0, 0.0));
	faces.set(2, godot::Vector3(0.0, 0.0, 1.0));
	godot::Ref<godot::ConcavePolygonShape3D> warmup;
	warmup.instantiate();
	warmup->set_faces(faces);
	godot::StaticBody3D *body = memnew(godot::StaticBody3D);
	godot::CollisionShape3D *shape = memnew(godot::CollisionShape3D);
	owner_.add_child(body);
	body->add_child(shape);
	shape->set_shape(warmup);
	owner_.remove_child(body);
	body->queue_free();
}

bool WtGodotCollisionSink::apply_collision(const WtCollisionPayload &payload) {
	if (!on_owner_thread()) return false;
	if (payload.faces.empty()) {
		const auto iterator = records_.find(payload.key);
		if (iterator != records_.end() &&
				should_stage_existing_replacement(payload.key)) {
			Record &record = iterator->second;
			record.staged_shape.unref();
			record.staged_generation = payload.generation;
			record.staged = true;
			record.staged_empty = true;
			return true;
		}
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

	const auto existing = records_.find(payload.key);
	const bool created = existing == records_.end();
	const bool stage = created ?
		should_stage_created_record(payload.key) :
		should_stage_existing_replacement(payload.key);
	Record &record = records_[payload.key];
	if (created) {
		record.body = memnew(godot::StaticBody3D);
		record.shape = memnew(godot::CollisionShape3D);
		record.body->set_name(chunk_name(payload.key));
		record.shape->set_name("Shape");
		record.body->add_child(record.shape);
	}
	record.body->set_position(to_godot(payload.world_origin));
	if (stage) {
		record.staged_shape = shape;
		record.staged_position = to_godot(payload.world_origin);
		record.staged_generation = payload.generation;
		record.staged = true;
		record.staged_empty = false;
		return true;
	}
	if (!record.active) {
		owner_.add_child(record.body);
		record.active = true;
	}
	record.shape->set_shape(shape);
	record.generation = payload.generation;
	record.staged_shape.unref();
	record.staged_generation = {};
	record.staged = false;
	record.staged_empty = false;
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
	if (iterator->second.active) {
		owner_.remove_child(iterator->second.body);
	}
	iterator->second.body->queue_free();
	records_.erase(iterator);
	return true;
}

void WtGodotCollisionSink::clear() {
	if (!on_owner_thread()) {
		return;
	}
	for (auto &entry : records_) {
		if (entry.second.active) {
			owner_.remove_child(entry.second.body);
		}
		entry.second.body->queue_free();
	}
	records_.clear();
}

std::size_t WtGodotCollisionSink::resource_count() const noexcept {
	std::size_t count = 0;
	for (const auto &entry : records_) {
		count += entry.second.active ? 1U : 0U;
	}
	return count;
}

std::size_t WtGodotCollisionSink::staged_count() const noexcept {
	std::size_t count = 0;
	for (const auto &entry : records_) {
		count += entry.second.staged ? 1U : 0U;
	}
	return count;
}

void WtGodotCollisionSink::set_new_record_staging_enabled(
	bool enabled
) noexcept {
	new_record_staging_enabled_ = enabled;
}

void WtGodotCollisionSink::set_staging_reference_chunks(
	const std::vector<WtChunkKey> &keys
) {
	staging_reference_chunks_ = keys;
}

bool WtGodotCollisionSink::has_staged_records() const noexcept {
	for (const auto &entry : records_) {
		if (entry.second.staged) return true;
	}
	return false;
}

bool WtGodotCollisionSink::publish_staged_record(
	const WtChunkKey &key
) noexcept {
	if (!on_owner_thread()) return false;
	const auto iterator = records_.find(key);
	if (iterator == records_.end() || !iterator->second.staged) return true;
	Record &record = iterator->second;
	if (record.body == nullptr || record.shape == nullptr) return false;
	if (record.staged_empty) {
		if (record.active) owner_.remove_child(record.body);
		record.body->queue_free();
		records_.erase(iterator);
		return true;
	}
	if (record.staged_shape.is_null()) return false;
	if (!record.active) {
		owner_.add_child(record.body);
		record.active = true;
	}
	record.body->set_position(record.staged_position);
	record.shape->set_shape(record.staged_shape);
	record.generation = record.staged_generation;
	record.staged_shape.unref();
	record.staged_generation = {};
	record.staged = false;
	record.staged_empty = false;
	return true;
}

void WtGodotCollisionSink::publish_staged_records() noexcept {
	if (!on_owner_thread()) return;
	std::vector<WtChunkKey> keys;
	keys.reserve(records_.size());
	for (const auto &entry : records_) {
		if (entry.second.staged) keys.push_back(entry.first);
	}
	for (const WtChunkKey &key : keys) {
		publish_staged_record(key);
	}
}

WtGenerationToken WtGodotCollisionSink::applied_generation(
	const WtChunkKey &key
) const noexcept {
	const auto iterator = records_.find(key);
	return iterator == records_.end() || !iterator->second.active ?
		WtGenerationToken{} : iterator->second.generation;
}

WtGenerationToken WtGodotCollisionSink::staged_generation(
	const WtChunkKey &key
) const noexcept {
	const auto iterator = records_.find(key);
	return iterator == records_.end() ? WtGenerationToken{} :
		iterator->second.staged_generation;
}

bool WtGodotCollisionSink::on_owner_thread() const noexcept {
	return std::this_thread::get_id() == owner_thread_;
}

bool WtGodotCollisionSink::should_stage_created_record(
	const WtChunkKey &key
) const noexcept {
	if (!new_record_staging_enabled_ || staging_reference_chunks_.empty()) {
		return false;
	}
	const WtChunkBounds bounds = wt_chunk_bounds(key);
	bool touches_replacement_region = false;
	for (const WtChunkKey &reference_key : staging_reference_chunks_) {
		const WtChunkBounds reference = wt_chunk_bounds(reference_key);
		const bool overlaps =
			bounds.minimum.x < reference.maximum.x &&
			reference.minimum.x < bounds.maximum.x &&
			bounds.minimum.y < reference.maximum.y &&
			reference.minimum.y < bounds.maximum.y &&
			bounds.minimum.z < reference.maximum.z &&
			reference.minimum.z < bounds.maximum.z;
		const bool touches_or_overlaps =
			bounds.minimum.x <= reference.maximum.x &&
			reference.minimum.x <= bounds.maximum.x &&
			bounds.minimum.y <= reference.maximum.y &&
			reference.minimum.y <= bounds.maximum.y &&
			bounds.minimum.z <= reference.maximum.z &&
			reference.minimum.z <= bounds.maximum.z;
		if (overlaps || (touches_or_overlaps && key.lod != reference_key.lod)) {
			touches_replacement_region = true;
			break;
		}
	}
	if (!touches_replacement_region) return false;
	for (const auto &entry : records_) {
		if (!entry.second.active || entry.first == key) continue;
		const WtChunkBounds active = wt_chunk_bounds(entry.first);
		const bool overlaps =
			bounds.minimum.x < active.maximum.x &&
			active.minimum.x < bounds.maximum.x &&
			bounds.minimum.y < active.maximum.y &&
			active.minimum.y < bounds.maximum.y &&
			bounds.minimum.z < active.maximum.z &&
			active.minimum.z < bounds.maximum.z;
		if (overlaps) return true;
	}
	return false;
}

bool WtGodotCollisionSink::should_stage_existing_replacement(
	const WtChunkKey &key
) const noexcept {
	(void)key;
	return new_record_staging_enabled_;
}

} // namespace world_transvoxel
