#pragma once

#include "render/wt_render_apply_queue.h"

#include <godot_cpp/classes/node3d.hpp>

#include <cstdint>
#include <map>
#include <thread>
#include <vector>

namespace godot {
class MeshInstance3D;
}

namespace world_transvoxel {

class WtGodotRenderSink final : public WtRenderSink {
public:
	explicit WtGodotRenderSink(godot::Node3D &owner) noexcept;

	bool apply_render(const WtRenderPayload &payload) override;
	bool remove_render(const WtChunkKey &key);
	bool begin_render_retirement(const WtChunkKey &key);
	void advance_retirements();
	void clear();
	std::size_t resource_count() const noexcept;
	std::size_t fading_count() const noexcept;
	WtGenerationToken applied_generation(const WtChunkKey &key) const noexcept;

private:
	struct Record {
		WtChunkKey key;
		godot::MeshInstance3D *instance = nullptr;
		WtGenerationToken generation;
		float current_transparency = 0.0F;
		float retirement_start_transparency = 0.0F;
		std::uint32_t introduction_frame = 0;
		std::uint32_t retirement_frame = 0;
		bool introducing = false;
		bool retiring = false;
	};

	bool on_owner_thread() const noexcept;
	static void set_record_transparency(Record &record, float value) noexcept;
	godot::Node3D &owner_;
	std::thread::id owner_thread_;
	std::map<WtChunkKey, Record> records_;
	std::vector<Record> replacement_retirements_;
};

} // namespace world_transvoxel
