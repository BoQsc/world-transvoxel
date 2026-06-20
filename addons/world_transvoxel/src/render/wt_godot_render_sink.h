#pragma once

#include "render/wt_render_apply_queue.h"

#include <godot_cpp/classes/node3d.hpp>

#include <map>
#include <thread>

namespace godot {
class MeshInstance3D;
}

namespace world_transvoxel {

class WtGodotRenderSink final : public WtRenderSink {
public:
	explicit WtGodotRenderSink(godot::Node3D &owner) noexcept;

	bool apply_render(const WtRenderPayload &payload) override;
	bool remove_render(const WtChunkKey &key);
	void clear();
	std::size_t resource_count() const noexcept;
	WtGenerationToken applied_generation(const WtChunkKey &key) const noexcept;

private:
	struct Record {
		godot::MeshInstance3D *instance = nullptr;
		WtGenerationToken generation;
	};

	bool on_owner_thread() const noexcept;
	godot::Node3D &owner_;
	std::thread::id owner_thread_;
	std::map<WtChunkKey, Record> records_;
};

} // namespace world_transvoxel
