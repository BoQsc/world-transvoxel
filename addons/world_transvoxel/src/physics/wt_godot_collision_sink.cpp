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
	if (payload.preserve_existing) {
		const auto iterator = records_.find(payload.key);
		if (iterator == records_.end() || !iterator->second.active) {
			const auto empty = empty_generations_.find(payload.key);
			if (empty == empty_generations_.end()) return false;
			empty->second = payload.generation;
			return true;
		}
		Record &record = iterator->second;
		record.generation = payload.generation;
		for (auto &shape : record.staged_shapes) shape.unref();
		record.staged_generation = {};
		record.staged = false;
		record.staged_empty = false;
		record.staged_dirty_block_mask = 0;
		return true;
	}
	const bool complete_empty = payload.faces.empty() &&
		payload.dirty_block_mask == kWtCollisionAllBlocksMask;
	if (complete_empty) {
		const auto iterator = records_.find(payload.key);
		if (!payload.incremental_patch && iterator != records_.end() &&
				should_stage_existing_replacement(payload.key)) {
			Record &record = iterator->second;
			for (auto &shape : record.staged_shapes) shape.unref();
			record.staged_generation = payload.generation;
			record.staged = true;
			record.staged_empty = true;
			record.staged_dirty_block_mask = kWtCollisionAllBlocksMask;
			return true;
		}
		remove_collision(payload.key);
		empty_generations_[payload.key] = payload.generation;
		return true;
	}
	const auto existing = records_.find(payload.key);
	const bool created = existing == records_.end();
	const auto empty_base = empty_generations_.find(payload.key);
	const bool extends_empty_generation = empty_base != empty_generations_.end() &&
		empty_base->second.value < payload.generation.value;
	if (created && payload.dirty_block_mask != kWtCollisionAllBlocksMask &&
		!extends_empty_generation) {
		return false;
	}
	empty_generations_.erase(payload.key);
	std::array<godot::Ref<godot::Shape3D>, kWtCollisionBlockCount>
		candidate_shapes{};
	for (std::size_t block = 0; block < payload.blocks.size(); ++block) {
		if ((payload.dirty_block_mask & (1U << block)) == 0) continue;
		const WtCollisionBlockRange &range = payload.blocks[block];
		if (range.face_count == 0) continue;
		godot::PackedVector3Array faces;
		faces.resize(static_cast<std::int64_t>(range.face_count));
		for (std::size_t triangle = 0; triangle < range.face_count; triangle += 3) {
			const std::size_t source = range.first_face + triangle;
			// Match Godot's clockwise front-face convention so the default
			// one-sided concave collision accepts rays and bodies from outside.
			faces.set(static_cast<std::int64_t>(triangle),
				to_godot(payload.faces[source]));
			faces.set(static_cast<std::int64_t>(triangle + 1),
				to_godot(payload.faces[source + 2]));
			faces.set(static_cast<std::int64_t>(triangle + 2),
				to_godot(payload.faces[source + 1]));
		}
		godot::Ref<godot::ConcavePolygonShape3D> shape;
		shape.instantiate();
		shape->set_faces(faces);
		candidate_shapes[block] = shape;
	}
	const bool stage = !payload.incremental_patch && (created ?
		should_stage_created_record(payload.key) :
		should_stage_existing_replacement(payload.key));
	Record &record = records_[payload.key];
	if (created) {
		record.body = memnew(godot::StaticBody3D);
		record.body->set_name(chunk_name(payload.key));
		for (std::size_t block = 0; block < record.shapes.size(); ++block) {
			record.shapes[block] = memnew(godot::CollisionShape3D);
			record.shapes[block]->set_name(block == 0 ? "Shape" :
				godot::String("Shape_") + godot::String::num_int64(block));
			record.body->add_child(record.shapes[block]);
		}
	}
	record.body->set_position(to_godot(payload.world_origin));
	if (stage) {
		for (auto &shape : record.staged_shapes) shape.unref();
		for (std::size_t block = 0; block < candidate_shapes.size(); ++block) {
			if ((payload.dirty_block_mask & (1U << block)) != 0) {
				record.staged_shapes[block] = candidate_shapes[block];
			}
		}
		record.staged_position = to_godot(payload.world_origin);
		record.staged_generation = payload.generation;
		record.staged = true;
		record.staged_empty = false;
		record.staged_dirty_block_mask = payload.dirty_block_mask;
		return true;
	}
	if (!record.active) {
		owner_.add_child(record.body);
		record.active = true;
	}
	for (std::size_t block = 0; block < candidate_shapes.size(); ++block) {
		if ((payload.dirty_block_mask & (1U << block)) != 0) {
			record.shapes[block]->set_shape(candidate_shapes[block]);
		}
	}
	record.generation = payload.generation;
	for (auto &shape : record.staged_shapes) shape.unref();
	record.staged_generation = {};
	record.staged = false;
	record.staged_empty = false;
	record.staged_dirty_block_mask = 0;
	return true;
}

bool WtGodotCollisionSink::remove_collision(const WtChunkKey &key) {
	if (!on_owner_thread()) {
		return false;
	}
	const auto iterator = records_.find(key);
	if (iterator == records_.end()) {
		return empty_generations_.erase(key) != 0;
	}
	if (iterator->second.active) {
		owner_.remove_child(iterator->second.body);
	}
	iterator->second.body->queue_free();
	records_.erase(iterator);
	empty_generations_.erase(key);
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
	empty_generations_.clear();
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

std::size_t WtGodotCollisionSink::empty_generation_count() const noexcept {
	return empty_generations_.size();
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

bool WtGodotCollisionSink::can_publish_staged_record(
	const WtChunkKey &key,
	WtGenerationToken generation
) const noexcept {
	const auto iterator = records_.find(key);
	if (iterator == records_.end()) return true;
	const Record &record = iterator->second;
	if (!record.staged) {
		return record.active && record.generation == generation;
	}
	if (record.body == nullptr || record.staged_generation != generation ||
			record.staged_dirty_block_mask == 0) {
		return false;
	}
	for (godot::CollisionShape3D *shape : record.shapes) {
		if (shape == nullptr) return false;
	}
	return true;
}

bool WtGodotCollisionSink::publish_staged_record(
	const WtChunkKey &key
) noexcept {
	if (!on_owner_thread()) return false;
	const auto iterator = records_.find(key);
	if (iterator == records_.end() || !iterator->second.staged) return true;
	Record &record = iterator->second;
	if (record.body == nullptr || record.staged_dirty_block_mask == 0) return false;
	if (record.staged_empty) {
		const WtGenerationToken empty_generation = record.staged_generation;
		if (record.active) owner_.remove_child(record.body);
		record.body->queue_free();
		records_.erase(iterator);
		empty_generations_[key] = empty_generation;
		return true;
	}
	if (!record.active) {
		owner_.add_child(record.body);
		record.active = true;
	}
	record.body->set_position(record.staged_position);
	for (std::size_t block = 0; block < record.shapes.size(); ++block) {
		if ((record.staged_dirty_block_mask & (1U << block)) != 0) {
			record.shapes[block]->set_shape(record.staged_shapes[block]);
		}
	}
	record.generation = record.staged_generation;
	for (auto &shape : record.staged_shapes) shape.unref();
	record.staged_generation = {};
	record.staged = false;
	record.staged_empty = false;
	record.staged_dirty_block_mask = 0;
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
	if (iterator != records_.end() && iterator->second.active) {
		return iterator->second.generation;
	}
	const auto empty = empty_generations_.find(key);
	return empty == empty_generations_.end() ? WtGenerationToken{} :
		empty->second;
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
