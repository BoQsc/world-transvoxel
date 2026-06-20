#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <memory>

namespace world_transvoxel {

class WtChunkApplicationService;
class WtGodotCollisionSink;
class WtGodotRenderSink;
class WtM3IntegrationFixture;

class WorldTransvoxelTerrain : public godot::Node3D {
	GDCLASS(WorldTransvoxelTerrain, godot::Node3D)

protected:
	static void _bind_methods();

public:
	WorldTransvoxelTerrain();
	~WorldTransvoxelTerrain() override;
	void _process(double delta) override;

	godot::String get_addon_version() const;
	godot::String get_milestone() const;
	bool is_mit_backend_available() const;
	godot::String get_backend_id() const;
	godot::String get_backend_license() const;
	godot::String get_backend_upstream_revision() const;
	void set_render_apply_budget(std::int64_t budget);
	std::int64_t get_render_apply_budget() const noexcept;
	void set_collision_apply_budget(std::int64_t budget);
	std::int64_t get_collision_apply_budget() const noexcept;
	std::int64_t get_render_resource_count() const noexcept;
	std::int64_t get_collision_resource_count() const noexcept;
	std::int64_t get_queued_render_count() const noexcept;
	std::int64_t get_queued_collision_count() const noexcept;
	std::int64_t get_render_latency_frames_maximum() const noexcept;
	std::int64_t get_collision_latency_frames_maximum() const noexcept;

	bool _m3_test_submit_generation(std::int64_t generation, bool collision_required);
	bool _m3_test_set_collision_distance(double distance);
	bool _m3_test_fully_ready() const noexcept;
	std::int64_t _m3_test_render_generation() const noexcept;
	std::int64_t _m3_test_collision_generation() const noexcept;
	std::int64_t _m3_test_stale_render_count() const noexcept;
	std::int64_t _m3_test_stale_collision_count() const noexcept;
	void _m3_test_forget_chunk();

private:
	std::unique_ptr<WtChunkApplicationService> application_;
	std::unique_ptr<WtGodotRenderSink> render_sink_;
	std::unique_ptr<WtGodotCollisionSink> collision_sink_;
	std::unique_ptr<WtM3IntegrationFixture> integration_fixture_;
	std::size_t render_apply_budget_ = 4;
	std::size_t collision_apply_budget_ = 2;
};

} // namespace world_transvoxel
