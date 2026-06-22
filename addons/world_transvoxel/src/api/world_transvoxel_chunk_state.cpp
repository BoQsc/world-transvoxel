#include "api/world_transvoxel_chunk_state.h"

#include "services/wt_chunk_application.h"

#include <godot_cpp/core/class_db.hpp>

namespace world_transvoxel {

void WorldTransvoxelChunkState::_bind_methods() {
	godot::ClassDB::bind_method(
		godot::D_METHOD("is_present"),
		&WorldTransvoxelChunkState::is_present
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_chunk_coordinate"),
		&WorldTransvoxelChunkState::get_chunk_coordinate
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_lod"),
		&WorldTransvoxelChunkState::get_lod
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_generation"),
		&WorldTransvoxelChunkState::get_generation
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("is_visual_ready"),
		&WorldTransvoxelChunkState::is_visual_ready
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("is_collision_required"),
		&WorldTransvoxelChunkState::is_collision_required
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("is_collision_ready"),
		&WorldTransvoxelChunkState::is_collision_ready
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("is_fully_ready"),
		&WorldTransvoxelChunkState::is_fully_ready
	);
}

bool WorldTransvoxelChunkState::is_present() const noexcept {
	return present_;
}

godot::Vector3i
WorldTransvoxelChunkState::get_chunk_coordinate() const noexcept {
	return { key_.x, key_.y, key_.z };
}

std::int64_t WorldTransvoxelChunkState::get_lod() const noexcept {
	return key_.lod;
}

std::int64_t WorldTransvoxelChunkState::get_generation() const noexcept {
	return static_cast<std::int64_t>(generation_.value);
}

bool WorldTransvoxelChunkState::is_visual_ready() const noexcept {
	return visual_ready_;
}

bool WorldTransvoxelChunkState::is_collision_required() const noexcept {
	return collision_required_;
}

bool WorldTransvoxelChunkState::is_collision_ready() const noexcept {
	return collision_ready_;
}

bool WorldTransvoxelChunkState::is_fully_ready() const noexcept {
	return visual_ready_ && (!collision_required_ || collision_ready_);
}

void WorldTransvoxelChunkState::set_snapshot(
	const WtChunkKey &key,
	const WtChunkApplicationRecord *record
) noexcept {
	key_ = key;
	present_ = record != nullptr;
	generation_ = record != nullptr ? record->generation : WtGenerationToken{};
	visual_ready_ = record != nullptr && record->visual_ready;
	collision_required_ = record != nullptr && record->collision_required;
	collision_ready_ = record != nullptr && record->collision_ready;
}

} // namespace world_transvoxel
