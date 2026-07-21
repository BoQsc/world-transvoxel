#pragma once

#include "core/wt_chunk_state.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <cstdint>

namespace world_transvoxel {

class WorldTransvoxelTerrain;
struct WtChunkApplicationRecord;

class WorldTransvoxelChunkState : public godot::RefCounted {
	GDCLASS(WorldTransvoxelChunkState, godot::RefCounted)

protected:
	static void _bind_methods();

public:
	bool is_present() const noexcept;
	godot::Vector3i get_chunk_coordinate() const noexcept;
	std::int64_t get_lod() const noexcept;
	std::int64_t get_generation() const noexcept;
	bool is_visual_ready() const noexcept;
	bool is_visual_required() const noexcept;
	bool is_collision_required() const noexcept;
	bool is_collision_ready() const noexcept;
	bool is_fully_ready() const noexcept;

private:
	friend class WorldTransvoxelTerrain;

	void set_snapshot(
		const WtChunkKey &key,
		const WtChunkApplicationRecord *record
	) noexcept;

	WtChunkKey key_;
	WtGenerationToken generation_;
	bool present_ = false;
	bool visual_ready_ = false;
	bool visual_required_ = false;
	bool collision_required_ = false;
	bool collision_ready_ = false;
};

} // namespace world_transvoxel
