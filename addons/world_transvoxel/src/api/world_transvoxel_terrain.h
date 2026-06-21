#pragma once

#include "api/world_transvoxel_config.h"
#include "services/wt_world_lifecycle.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <memory>

namespace world_transvoxel {

class WtChunkApplicationService;
class WtGodotCollisionSink;
class WtGodotRenderSink;
class WtM3IntegrationFixture;
class WtM5ApplicationBenchmarkFixture;

constexpr std::size_t kWtDefaultRenderApplyBudget = 4;
constexpr std::size_t kWtDefaultCollisionApplyBudget = 2;

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
	void set_configuration(
		const godot::Ref<WorldTransvoxelConfig> &configuration
	);
	godot::Ref<WorldTransvoxelConfig> get_configuration() const;
	bool is_configuration_valid() const noexcept;
	godot::String get_configuration_error() const;
	bool start_world(
		const godot::String &world_manifest_path,
		const godot::String &object_root
	);
	bool stop_world();
	std::int64_t get_world_state() const noexcept;
	godot::String get_world_state_name() const;
	bool is_world_running() const noexcept;
	godot::String get_world_error() const;
	std::int64_t get_world_source_revision() const noexcept;
	std::int64_t get_world_revision() const noexcept;
	std::int64_t get_world_page_count() const noexcept;
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
	bool _m5_benchmark_prepare_batch(
		std::int64_t render_count,
		std::int64_t collision_count,
		std::int64_t generation
	);
	godot::Dictionary _m5_benchmark_apply_frame();
	std::int64_t _m5_benchmark_clear();

private:
	void emit_lifecycle_state(WtWorldLifecycleState state);
	void notify_lifecycle_state();

	godot::Ref<WorldTransvoxelConfig> configuration_;
	std::unique_ptr<WtWorldLifecycleService> lifecycle_;
	WtWorldLifecycleState last_notified_state_ =
		WtWorldLifecycleState::Stopped;
	godot::String synchronous_world_error_ = "ok";
	std::unique_ptr<WtChunkApplicationService> application_;
	std::unique_ptr<WtGodotRenderSink> render_sink_;
	std::unique_ptr<WtGodotCollisionSink> collision_sink_;
	std::unique_ptr<WtM3IntegrationFixture> integration_fixture_;
	std::unique_ptr<WtM5ApplicationBenchmarkFixture> application_benchmark_;
	std::size_t render_apply_budget_ = kWtDefaultRenderApplyBudget;
	std::size_t collision_apply_budget_ = kWtDefaultCollisionApplyBudget;
};

} // namespace world_transvoxel
