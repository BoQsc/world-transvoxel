#pragma once

#include "api/world_transvoxel_chunk_state.h"
#include "api/world_transvoxel_config.h"
#include "api/world_transvoxel_edit_transaction.h"
#include "api/world_transvoxel_sample.h"
#include "services/wt_world_lifecycle.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector3i.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <memory>
#include <vector>

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
	static void bind_edit_methods();
	static void bind_metrics_methods();
	static void bind_query_snapshot_methods();

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
	bool start_procedural_world(
		std::int64_t chunk_count_x,
		std::int64_t chunk_count_z,
		std::int64_t seed,
		std::int64_t source_revision,
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
	godot::Ref<WorldTransvoxelEditTransaction> begin_edit_transaction(
		std::int64_t author_id = 0
	);
	bool commit_edit_transaction(
		const godot::Ref<WorldTransvoxelEditTransaction> &transaction
	);
	bool update_viewer(
		std::int64_t viewer_id,
		std::int64_t revision,
		const godot::Vector3 &position,
		std::int64_t radius_chunks,
		std::int64_t maximum_lod = 0
	);
	bool remove_viewer(std::int64_t viewer_id, std::int64_t revision);
	godot::Ref<WorldTransvoxelChunkState> query_chunk_state(
		const godot::Vector3i &chunk_coordinate,
		std::int64_t lod
	) const;
	std::int64_t request_authoritative_sample(
		const godot::Vector3i &grid_point,
		std::int64_t lod = 0
	);
	std::int64_t request_authoritative_samples(
		const godot::Array &grid_points,
		std::int64_t lod = 0
	);
	std::int64_t request_world_compaction(
		const godot::String &output_directory,
		std::int64_t new_source_revision
	);
	std::int64_t request_world_migration(
		const godot::String &output_directory
	);
	std::int64_t get_rendered_chunk_count() const noexcept;
	std::int64_t get_collision_chunk_count() const noexcept;
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
	godot::Dictionary get_runtime_metrics() const;

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
	void drain_world_publications();
	void stage_chunk_retirement(const WtChunkKey &key);
	void cancel_chunk_retirement(const WtChunkKey &key);
	void flush_ready_chunk_retirements();
	void reset_world_application(std::size_t capacity);

	godot::Ref<WorldTransvoxelConfig> configuration_;
	std::unique_ptr<WtWorldLifecycleService> lifecycle_;
	WtWorldLifecycleState last_notified_state_ =
		WtWorldLifecycleState::Stopped;
	godot::String synchronous_world_error_ = "ok";
	WtReadOnlyPublication deferred_publication_;
	bool has_deferred_publication_ = false;
	std::vector<WtChunkKey> pending_chunk_retirements_;
	std::unique_ptr<WtChunkApplicationService> application_;
	std::unique_ptr<WtGodotRenderSink> render_sink_;
	std::unique_ptr<WtGodotCollisionSink> collision_sink_;
	std::unique_ptr<WtM3IntegrationFixture> integration_fixture_;
	std::unique_ptr<WtM5ApplicationBenchmarkFixture> application_benchmark_;
	std::size_t render_apply_budget_ = kWtDefaultRenderApplyBudget;
	std::size_t collision_apply_budget_ = kWtDefaultCollisionApplyBudget;
};

} // namespace world_transvoxel
