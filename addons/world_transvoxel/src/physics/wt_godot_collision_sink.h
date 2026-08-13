#pragma once

#include "physics/wt_collision_apply_queue.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shape3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <map>
#include <thread>
#include <vector>

namespace godot {
class CollisionShape3D;
class StaticBody3D;
}

namespace world_transvoxel {

class WtGodotCollisionSink final : public WtCollisionSink {
public:
	explicit WtGodotCollisionSink(godot::Node3D &owner) noexcept;

	bool apply_collision(const WtCollisionPayload &payload) override;
	bool remove_collision(const WtChunkKey &key);
	void clear();
	std::size_t resource_count() const noexcept;
	std::size_t staged_count() const noexcept;
	void set_new_record_staging_enabled(bool enabled) noexcept;
	void set_staging_reference_chunks(const std::vector<WtChunkKey> &keys);
	bool has_staged_records() const noexcept;
	bool can_publish_staged_record(
		const WtChunkKey &key,
		WtGenerationToken generation
	) const noexcept;
	bool publish_staged_record(const WtChunkKey &key) noexcept;
	void publish_staged_records() noexcept;
	WtGenerationToken applied_generation(const WtChunkKey &key) const noexcept;
	WtGenerationToken staged_generation(const WtChunkKey &key) const noexcept;

private:
	struct Record {
		godot::StaticBody3D *body = nullptr;
		godot::CollisionShape3D *shape = nullptr;
		godot::Ref<godot::Shape3D> staged_shape;
		godot::Vector3 staged_position;
		WtGenerationToken generation;
		WtGenerationToken staged_generation;
		bool active = false;
		bool staged = false;
		bool staged_empty = false;
	};

	bool on_owner_thread() const noexcept;
	bool should_stage_created_record(const WtChunkKey &key) const noexcept;
	bool should_stage_existing_replacement(const WtChunkKey &key) const noexcept;
	godot::Node3D &owner_;
	std::thread::id owner_thread_;
	std::map<WtChunkKey, Record> records_;
	std::vector<WtChunkKey> staging_reference_chunks_;
	bool new_record_staging_enabled_ = false;
};

} // namespace world_transvoxel
