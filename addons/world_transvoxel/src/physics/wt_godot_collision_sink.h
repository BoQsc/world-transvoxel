#pragma once

#include "physics/wt_collision_apply_queue.h"

#include <godot_cpp/classes/node3d.hpp>

#include <map>
#include <thread>

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
	WtGenerationToken applied_generation(const WtChunkKey &key) const noexcept;

private:
	struct Record {
		godot::StaticBody3D *body = nullptr;
		godot::CollisionShape3D *shape = nullptr;
		WtGenerationToken generation;
	};

	bool on_owner_thread() const noexcept;
	godot::Node3D &owner_;
	std::thread::id owner_thread_;
	std::map<WtChunkKey, Record> records_;
};

} // namespace world_transvoxel
