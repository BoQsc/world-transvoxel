#include "api/world_transvoxel_terrain.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "core/wt_version.h"
#include "physics/wt_godot_collision_sink.h"
#include "render/wt_godot_render_sink.h"
#include "services/wt_chunk_application.h"
#include "testing/wt_m3_integration_fixture.h"
#include "testing/wt_m5_application_benchmark_fixture.h"

#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>

namespace world_transvoxel {

WorldTransvoxelTerrain::WorldTransvoxelTerrain() {
	application_ = std::make_unique<WtChunkApplicationService>(256, 128, 128);
	render_sink_ = std::make_unique<WtGodotRenderSink>(*this);
	collision_sink_ = std::make_unique<WtGodotCollisionSink>(*this);
	integration_fixture_ = std::make_unique<WtM3IntegrationFixture>();
	application_benchmark_ =
		std::make_unique<WtM5ApplicationBenchmarkFixture>();
	set_process(true);
}

WorldTransvoxelTerrain::~WorldTransvoxelTerrain() = default;

void WorldTransvoxelTerrain::_bind_methods() {
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_addon_version"),
		&WorldTransvoxelTerrain::get_addon_version
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_milestone"),
		&WorldTransvoxelTerrain::get_milestone
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("is_mit_backend_available"),
		&WorldTransvoxelTerrain::is_mit_backend_available
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_backend_id"),
		&WorldTransvoxelTerrain::get_backend_id
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_backend_license"),
		&WorldTransvoxelTerrain::get_backend_license
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_backend_upstream_revision"),
		&WorldTransvoxelTerrain::get_backend_upstream_revision
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("set_configuration", "configuration"),
		&WorldTransvoxelTerrain::set_configuration
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_configuration"),
		&WorldTransvoxelTerrain::get_configuration
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("is_configuration_valid"),
		&WorldTransvoxelTerrain::is_configuration_valid
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_configuration_error"),
		&WorldTransvoxelTerrain::get_configuration_error
	);
	ADD_PROPERTY(
		godot::PropertyInfo(
			godot::Variant::OBJECT,
			"configuration",
			godot::PROPERTY_HINT_RESOURCE_TYPE,
			"WorldTransvoxelConfig"
		),
		"set_configuration",
		"get_configuration"
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("start_world", "world_manifest_path", "object_root"),
		&WorldTransvoxelTerrain::start_world
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"start_procedural_world",
			"chunk_count_x",
			"chunk_count_z",
			"seed",
			"source_revision",
			"object_root"
		),
		&WorldTransvoxelTerrain::start_procedural_world
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("stop_world"),
		&WorldTransvoxelTerrain::stop_world
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_world_state"),
		&WorldTransvoxelTerrain::get_world_state
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_world_state_name"),
		&WorldTransvoxelTerrain::get_world_state_name
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("is_world_running"),
		&WorldTransvoxelTerrain::is_world_running
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_world_error"),
		&WorldTransvoxelTerrain::get_world_error
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_world_source_revision"),
		&WorldTransvoxelTerrain::get_world_source_revision
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_world_revision"),
		&WorldTransvoxelTerrain::get_world_revision
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_world_page_count"),
		&WorldTransvoxelTerrain::get_world_page_count
	);
	bind_edit_methods();
	bind_metrics_methods();
	bind_query_snapshot_methods();
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"update_viewer", "viewer_id", "revision", "position",
			"radius_chunks", "maximum_lod"
		),
		&WorldTransvoxelTerrain::update_viewer,
		DEFVAL(0)
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("remove_viewer", "viewer_id", "revision"),
		&WorldTransvoxelTerrain::remove_viewer
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("query_chunk_state", "chunk_coordinate", "lod"),
		&WorldTransvoxelTerrain::query_chunk_state
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_rendered_chunk_count"),
		&WorldTransvoxelTerrain::get_rendered_chunk_count
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_collision_chunk_count"),
		&WorldTransvoxelTerrain::get_collision_chunk_count
	);
	ADD_SIGNAL(godot::MethodInfo(
		"world_state_changed",
		godot::PropertyInfo(godot::Variant::INT, "state"),
		godot::PropertyInfo(godot::Variant::STRING, "state_name")
	));
	ADD_SIGNAL(godot::MethodInfo(
		"world_failed",
		godot::PropertyInfo(godot::Variant::STRING, "error")
	));
	godot::ClassDB::bind_method(
		godot::D_METHOD("set_render_apply_budget", "budget"),
		&WorldTransvoxelTerrain::set_render_apply_budget
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_render_apply_budget"),
		&WorldTransvoxelTerrain::get_render_apply_budget
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("set_collision_apply_budget", "budget"),
		&WorldTransvoxelTerrain::set_collision_apply_budget
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_collision_apply_budget"),
		&WorldTransvoxelTerrain::get_collision_apply_budget
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_render_resource_count"),
		&WorldTransvoxelTerrain::get_render_resource_count
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_collision_resource_count"),
		&WorldTransvoxelTerrain::get_collision_resource_count
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_queued_render_count"),
		&WorldTransvoxelTerrain::get_queued_render_count
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_queued_collision_count"),
		&WorldTransvoxelTerrain::get_queued_collision_count
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_render_latency_frames_maximum"),
		&WorldTransvoxelTerrain::get_render_latency_frames_maximum
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_collision_latency_frames_maximum"),
		&WorldTransvoxelTerrain::get_collision_latency_frames_maximum
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("_m3_test_submit_generation", "generation", "collision_required"),
		&WorldTransvoxelTerrain::_m3_test_submit_generation
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("_m3_test_set_collision_distance", "distance"),
		&WorldTransvoxelTerrain::_m3_test_set_collision_distance
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("_m3_test_fully_ready"),
		&WorldTransvoxelTerrain::_m3_test_fully_ready
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("_m3_test_render_generation"),
		&WorldTransvoxelTerrain::_m3_test_render_generation
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("_m3_test_collision_generation"),
		&WorldTransvoxelTerrain::_m3_test_collision_generation
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("_m3_test_stale_render_count"),
		&WorldTransvoxelTerrain::_m3_test_stale_render_count
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("_m3_test_stale_collision_count"),
		&WorldTransvoxelTerrain::_m3_test_stale_collision_count
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("_m3_test_forget_chunk"),
		&WorldTransvoxelTerrain::_m3_test_forget_chunk
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"_m5_benchmark_prepare_batch",
			"render_count",
			"collision_count",
			"generation"
		),
		&WorldTransvoxelTerrain::_m5_benchmark_prepare_batch
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("_m5_benchmark_apply_frame"),
		&WorldTransvoxelTerrain::_m5_benchmark_apply_frame
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("_m5_benchmark_clear"),
		&WorldTransvoxelTerrain::_m5_benchmark_clear
	);
}

godot::String WorldTransvoxelTerrain::get_addon_version() const {
	return kAddonVersion;
}

godot::String WorldTransvoxelTerrain::get_milestone() const {
	return kMilestone;
}

bool WorldTransvoxelTerrain::is_mit_backend_available() const {
	return wt_get_transvoxel_mit_backend().is_available();
}

godot::String WorldTransvoxelTerrain::get_backend_id() const {
	return wt_get_transvoxel_mit_backend().get_info().id;
}

godot::String WorldTransvoxelTerrain::get_backend_license() const {
	return wt_get_transvoxel_mit_backend().get_info().license;
}

godot::String WorldTransvoxelTerrain::get_backend_upstream_revision() const {
	return wt_get_transvoxel_mit_backend().get_info().upstream_revision;
}

void WorldTransvoxelTerrain::set_configuration(
	const godot::Ref<WorldTransvoxelConfig> &configuration
) {
	if (lifecycle_ &&
		lifecycle_->state() != WtWorldLifecycleState::Stopped) {
		return;
	}
	configuration_ = configuration;
}

godot::Ref<WorldTransvoxelConfig>
WorldTransvoxelTerrain::get_configuration() const {
	return configuration_;
}

bool WorldTransvoxelTerrain::is_configuration_valid() const noexcept {
	return configuration_.is_valid() && configuration_->is_valid();
}

godot::String WorldTransvoxelTerrain::get_configuration_error() const {
	return configuration_.is_valid() ?
		configuration_->get_validation_error() :
		godot::String("configuration is required");
}

bool WorldTransvoxelTerrain::stop_world() {
	if (!lifecycle_) {
		synchronous_world_error_ = "world is already stopped";
		return false;
	}
	const WtWorldLifecycleStatus status = lifecycle_->request_stop();
	if (status != WtWorldLifecycleStatus::Ok &&
		status != WtWorldLifecycleStatus::AlreadyStopping) {
		synchronous_world_error_ =
			wt_world_lifecycle_status_message(status);
		return false;
	}
	synchronous_world_error_ = "ok";
	emit_lifecycle_state(WtWorldLifecycleState::Stopping);
	return true;
}

std::int64_t WorldTransvoxelTerrain::get_world_state() const noexcept {
	return static_cast<std::int64_t>(
		lifecycle_ ? lifecycle_->state() : WtWorldLifecycleState::Stopped
	);
}

godot::String WorldTransvoxelTerrain::get_world_state_name() const {
	const WtWorldLifecycleState state = lifecycle_ ?
		lifecycle_->state() : WtWorldLifecycleState::Stopped;
	return wt_world_lifecycle_state_name(state);
}

bool WorldTransvoxelTerrain::is_world_running() const noexcept {
	return lifecycle_ &&
		lifecycle_->state() == WtWorldLifecycleState::Running;
}

godot::String WorldTransvoxelTerrain::get_world_error() const {
	if (lifecycle_ &&
		lifecycle_->last_storage_status() != WtAsyncStorageStatus::Ok) {
		return wt_async_storage_status_message(
			lifecycle_->last_storage_status()
		);
	}
	if (lifecycle_ &&
		lifecycle_->last_edit_journal_status() !=
			WtEditJournalStoreStatus::Ok) {
		return wt_edit_journal_store_status_message(
			lifecycle_->last_edit_journal_status()
		);
	}
	if (lifecycle_ &&
		lifecycle_->last_runtime_status() != WtReadOnlyRuntimeStatus::Ok) {
		return wt_read_only_runtime_status_message(
			lifecycle_->last_runtime_status()
		);
	}
	return synchronous_world_error_;
}

std::int64_t
WorldTransvoxelTerrain::get_world_source_revision() const noexcept {
	return static_cast<std::int64_t>(
		lifecycle_ ? lifecycle_->source_revision() : 0
	);
}

std::int64_t WorldTransvoxelTerrain::get_world_revision() const noexcept {
	return static_cast<std::int64_t>(
		lifecycle_ ? lifecycle_->world_revision() : 0
	);
}

std::int64_t WorldTransvoxelTerrain::get_world_page_count() const noexcept {
	return static_cast<std::int64_t>(
		lifecycle_ ? lifecycle_->page_count() : 0
	);
}

void WorldTransvoxelTerrain::notify_lifecycle_state() {
	const WtWorldLifecycleState state = lifecycle_ ?
		lifecycle_->state() : WtWorldLifecycleState::Stopped;
	emit_lifecycle_state(state);
}

void WorldTransvoxelTerrain::emit_lifecycle_state(
	WtWorldLifecycleState state
) {
	if (state == last_notified_state_) return;
	last_notified_state_ = state;
	if (state == WtWorldLifecycleState::Stopped) {
		render_sink_->clear();
		collision_sink_->clear();
		const std::size_t capacity = configuration_.is_valid() ?
			static_cast<std::size_t>(
				configuration_->to_native().active_chunk_capacity
			) : 256U;
		reset_world_application(capacity);
	}
	emit_signal(
		"world_state_changed",
		static_cast<std::int64_t>(state),
		godot::String(wt_world_lifecycle_state_name(state))
	);
	if (state == WtWorldLifecycleState::Failed) {
		emit_signal("world_failed", get_world_error());
	}
}

void WorldTransvoxelTerrain::set_render_apply_budget(std::int64_t budget) {
	render_apply_budget_ = static_cast<std::size_t>(
		std::clamp<std::int64_t>(budget, 0, 128)
	);
}

std::int64_t WorldTransvoxelTerrain::get_render_apply_budget() const noexcept {
	return static_cast<std::int64_t>(render_apply_budget_);
}

void WorldTransvoxelTerrain::set_collision_apply_budget(std::int64_t budget) {
	collision_apply_budget_ = static_cast<std::size_t>(
		std::clamp<std::int64_t>(budget, 0, 128)
	);
}

std::int64_t WorldTransvoxelTerrain::get_collision_apply_budget() const noexcept {
	return static_cast<std::int64_t>(collision_apply_budget_);
}

std::int64_t WorldTransvoxelTerrain::get_render_resource_count() const noexcept {
	return static_cast<std::int64_t>(render_sink_->resource_count());
}

std::int64_t WorldTransvoxelTerrain::get_collision_resource_count() const noexcept {
	return static_cast<std::int64_t>(collision_sink_->resource_count());
}

std::int64_t WorldTransvoxelTerrain::get_queued_render_count() const noexcept {
	return static_cast<std::int64_t>(application_->queued_render_count());
}

std::int64_t WorldTransvoxelTerrain::get_queued_collision_count() const noexcept {
	return static_cast<std::int64_t>(application_->queued_collision_count());
}

std::int64_t WorldTransvoxelTerrain::get_render_latency_frames_maximum() const noexcept {
	return static_cast<std::int64_t>(
		application_->get_metrics().render_latency_frames_maximum
	);
}

std::int64_t WorldTransvoxelTerrain::get_collision_latency_frames_maximum() const noexcept {
	return static_cast<std::int64_t>(
		application_->get_metrics().collision_latency_frames_maximum
	);
}

bool WorldTransvoxelTerrain::_m3_test_submit_generation(
	std::int64_t generation,
	bool collision_required
) {
	return integration_fixture_->submit_generation(
		generation, collision_required, *application_
	);
}

bool WorldTransvoxelTerrain::_m3_test_set_collision_distance(double distance) {
	return integration_fixture_->set_collision_distance(
		distance, *application_, *collision_sink_
	);
}

bool WorldTransvoxelTerrain::_m3_test_fully_ready() const noexcept {
	return integration_fixture_->fully_ready(*application_);
}

std::int64_t WorldTransvoxelTerrain::_m3_test_render_generation() const noexcept {
	return integration_fixture_->render_generation(*render_sink_);
}

std::int64_t WorldTransvoxelTerrain::_m3_test_collision_generation() const noexcept {
	return integration_fixture_->collision_generation(*collision_sink_);
}

std::int64_t WorldTransvoxelTerrain::_m3_test_stale_render_count() const noexcept {
	return integration_fixture_->stale_render_count(*application_);
}

std::int64_t WorldTransvoxelTerrain::_m3_test_stale_collision_count() const noexcept {
	return integration_fixture_->stale_collision_count(*application_);
}

void WorldTransvoxelTerrain::_m3_test_forget_chunk() {
	integration_fixture_->forget(*application_, *render_sink_, *collision_sink_);
}

bool WorldTransvoxelTerrain::_m5_benchmark_prepare_batch(
	std::int64_t render_count,
	std::int64_t collision_count,
	std::int64_t generation
) {
	if (render_count <= 0 || collision_count < 0 || generation <= 0) {
		return false;
	}
	return application_benchmark_->prepare_batch(
		static_cast<std::size_t>(render_count),
		static_cast<std::size_t>(collision_count),
		static_cast<std::uint64_t>(generation),
		*application_
	);
}

godot::Dictionary WorldTransvoxelTerrain::_m5_benchmark_apply_frame() {
	const WtM5ApplicationFrameResult result =
		application_benchmark_->apply_frame(
			kWtDefaultRenderApplyBudget,
			kWtDefaultCollisionApplyBudget,
			*application_,
			*render_sink_,
			*collision_sink_
		);
	godot::Dictionary output;
	output["valid"] = result.valid;
	output["duration_ns"] = static_cast<std::int64_t>(result.duration_ns);
	output["render_sink_ns"] =
		static_cast<std::int64_t>(result.render_sink_ns);
	output["collision_sink_ns"] =
		static_cast<std::int64_t>(result.collision_sink_ns);
	output["render_processed"] =
		static_cast<std::int64_t>(result.render_processed);
	output["collision_processed"] =
		static_cast<std::int64_t>(result.collision_processed);
	output["ready_count"] = static_cast<std::int64_t>(result.ready_count);
	return output;
}

std::int64_t WorldTransvoxelTerrain::_m5_benchmark_clear() {
	return static_cast<std::int64_t>(application_benchmark_->clear(
		*application_,
		*render_sink_,
		*collision_sink_
	));
}

} // namespace world_transvoxel
