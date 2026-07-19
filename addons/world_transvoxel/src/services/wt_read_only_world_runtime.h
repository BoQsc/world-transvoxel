#pragma once

#include "physics/wt_collision_apply_queue.h"
#include "render/wt_render_apply_queue.h"
#include "services/wt_authoritative_sample_query.h"
#include "services/wt_runtime_config.h"
#include "storage/wt_world_snapshot_store.h"
#include "streaming/wt_balanced_lod_planner.h"
#include "editing/wt_edit_transaction.h"
#include "streaming/wt_multi_viewer_desired_set.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

namespace world_transvoxel {

constexpr std::size_t kWtProductionWorldOperationCapacity = 16;

class WtAsyncStorageService;
class WtChunkApplicationService;
class WtChunkResourceCache;
class WtDesiredSetRuntimeService;
class WtEditJournalStore;
class WtEditRuntimeReplacementService;
class WtEditSpatialIndex;
class WtChunkMesher;
struct WtChunkMeshingScratch;
struct WtTerrainMeshCompletion;
class WtPageMeshingRuntimeService;
class WtStoragePageCache;
class WtStreamScheduler;

enum class WtReadOnlyRuntimeStatus : std::uint8_t {
	Ok,
	InvalidConfiguration,
	NotRunning,
	InvalidViewer,
	ViewerQueueFull,
	InvalidEdit,
	EditQueueFull,
	EditFailure,
	InvalidQuery,
	InvalidSnapshot,
	OperationQueueFull,
	DesiredSetFailure,
	RuntimeDeltaFailure,
	RuntimeDeltaChangeCapacityExceeded,
	RuntimeDeltaStateMismatch,
	RuntimeDeltaRecordCapacityExceeded,
	RuntimeDeltaJobQueueCapacityExceeded,
	RuntimeDeltaSchedulerFailure,
	RuntimeDeltaApplicationFailure,
	RuntimeDeltaPageMeshingRuntimeFailure,
	PipelineFailure,
	PublicationFailure,
};

enum class WtReadOnlyPublicationKind : std::uint8_t {
	ExpectChunk,
	SetCollisionRequired,
	RemoveChunk,
	RenderPayload,
	CollisionPayload,
	EditCommitted,
	EditRejected,
	AuthoritativeSampleReady,
	AuthoritativeSampleRejected,
	AuthoritativeSampleBatchReady,
	AuthoritativeSampleBatchRejected,
	WorldSnapshotReady,
	WorldSnapshotRejected,
};

enum class WtReadOnlyEditStatus : std::uint8_t {
	Ok,
	InvalidTransaction,
	StaleRevision,
	SpatialFailure,
	JournalFailure,
	ReplacementFailure,
};

struct WtReadOnlyPublication {
	WtReadOnlyPublicationKind kind =
		WtReadOnlyPublicationKind::ExpectChunk;
	WtChunkKey key;
	WtGenerationToken generation;
	bool collision_required = false;
	WtRenderPayloadPtr render;
	WtCollisionPayloadPtr collision;
	std::uint64_t world_revision = 0;
	WtReadOnlyEditStatus edit_status = WtReadOnlyEditStatus::Ok;
	bool staged_replacement = false;
	std::uint64_t request_id = 0;
	WtAuthoritativeSampleQueryStatus sample_status =
		WtAuthoritativeSampleQueryStatus::Ok;
	WtAuthoritativeSample authoritative_sample;
	std::vector<WtAuthoritativeSample> authoritative_samples;
	WtWorldSnapshotStoreStatus snapshot_status =
		WtWorldSnapshotStoreStatus::Ok;
	std::filesystem::path snapshot_manifest_path;
	std::uint64_t snapshot_source_revision = 0;
	std::size_t snapshot_page_count = 0;
};

struct WtReadOnlyRuntimeMetrics {
	std::uint64_t viewer_updates = 0;
	std::uint64_t viewer_removals = 0;
	std::uint64_t coalesced_viewer_events = 0;
	std::uint64_t planned_demands = 0;
	std::uint64_t sample_jobs = 0;
	std::uint64_t mesh_jobs = 0;
	std::uint64_t storage_completions = 0;
	std::uint64_t mesh_completions = 0;
	std::uint64_t transition_mesh_completions = 0;
	std::uint64_t edit_commits = 0;
	std::uint64_t edit_rejections = 0;
	std::uint64_t edit_replacements = 0;
	std::uint64_t edit_lod_retention_zones = 0;
	std::uint64_t edit_lod_retention_active_viewers = 0;
	std::uint64_t edit_lod_retention_plans = 0;
	std::uint64_t edit_lod_retention_fallbacks = 0;
	std::uint64_t sample_queries = 0;
	std::uint64_t sample_query_rejections = 0;
	std::uint64_t world_snapshots = 0;
	std::uint64_t world_snapshot_rejections = 0;
	std::uint64_t published_events = 0;
	std::uint64_t rejected_events = 0;
	std::uint64_t scheduler_requested_records = 0;
	std::uint64_t scheduler_sampling_records = 0;
	std::uint64_t scheduler_meshing_records = 0;
	std::uint64_t scheduler_ready_records = 0;
	std::uint64_t scheduler_failed_records = 0;
	std::uint64_t scheduler_queued_jobs = 0;
	std::uint64_t scheduler_queued_completions = 0;
	std::uint64_t scheduler_queue_rejections = 0;
	std::uint64_t page_sample_failures = 0;
	std::uint64_t page_mesh_failures = 0;
	std::uint64_t page_storage_failures = 0;
	std::uint64_t page_cache_failures = 0;
	std::uint64_t page_scheduler_backpressure = 0;
	std::int64_t page_last_failure_key_x = 0;
	std::int64_t page_last_failure_key_y = 0;
	std::int64_t page_last_failure_key_z = 0;
	std::uint64_t page_last_failure_key_lod = 0;
};

class WtReadOnlyWorldRuntime {
public:
	WtReadOnlyWorldRuntime(
		WtRuntimeConfig config,
		WtAsyncStorageService &storage,
		WtEditJournalStore *edit_journal_store = nullptr
	);
	~WtReadOnlyWorldRuntime();

	WtReadOnlyWorldRuntime(const WtReadOnlyWorldRuntime &) = delete;
	WtReadOnlyWorldRuntime &operator=(
		const WtReadOnlyWorldRuntime &
	) = delete;

	bool valid() const noexcept;
	WtReadOnlyRuntimeStatus update_viewer(
		const WtViewerSnapshot &snapshot,
		std::uint32_t radius_chunks,
		std::uint8_t maximum_lod = 0
	);
	WtReadOnlyRuntimeStatus remove_viewer(
		std::uint64_t viewer_id,
		std::uint64_t revision
	);
	WtReadOnlyRuntimeStatus submit_edit(
		const WtEditTransaction &transaction
	);
	WtReadOnlyRuntimeStatus request_authoritative_sample(
		const WtGridPoint &point,
		std::uint8_t lod,
		std::uint64_t &request_id
	);
	WtReadOnlyRuntimeStatus request_authoritative_samples(
		const std::vector<WtGridPoint> &points,
		std::uint8_t lod,
		std::uint64_t &request_id
	);
	WtReadOnlyRuntimeStatus request_world_snapshot(
		const std::filesystem::path &output_directory,
		std::uint64_t new_source_revision,
		bool compact,
		std::uint64_t &request_id
	);
	WtReadOnlyRuntimeStatus run();
	void request_stop() noexcept;
	bool pop_publication(WtReadOnlyPublication &publication);

	WtReadOnlyRuntimeStatus last_status() const noexcept;
	WtReadOnlyRuntimeMetrics get_metrics() const noexcept;
	std::uint64_t world_revision() const noexcept;

private:
	enum class ViewerEventKind : std::uint8_t { Update, Remove };
	struct ViewerEvent {
		ViewerEventKind kind = ViewerEventKind::Update;
		WtViewerSnapshot snapshot;
		std::uint32_t radius_chunks = 0;
		std::uint8_t maximum_lod = 0;
	};
	enum class WorldOperationKind : std::uint8_t {
		Edit,
		AuthoritativeSample,
		AuthoritativeSampleBatch,
		CompactSnapshot,
		MigrateSnapshot,
	};
	struct WorldOperation {
		WorldOperationKind kind = WorldOperationKind::Edit;
		WtEditTransaction transaction;
		WtGridPoint point;
		std::vector<WtGridPoint> points;
		std::uint8_t lod = 0;
		std::uint64_t request_id = 0;
		std::filesystem::path output_directory;
		std::uint64_t new_source_revision = 0;
	};
	struct EditLodRetentionZone {
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
		WtGridPoint minimum;
		WtGridPoint maximum;
		std::uint32_t refinement_radius_chunks = 1;
		std::uint64_t revision = 0;
		std::uint64_t viewer_id = 0;
	};

	bool enqueue_viewer_event(const ViewerEvent &event);
	bool process_viewer_event();
	bool enqueue_world_operation(WorldOperation &operation);
	bool process_world_operation_event();
	bool process_edit_operation(const WtEditTransaction &transaction);
	bool process_sample_query_operation(const WorldOperation &operation);
	bool process_sample_batch_query_operation(const WorldOperation &operation);
	bool process_snapshot_operation(const WorldOperation &operation);
	bool process_storage_completions();
	bool process_pending_transition_remeshes();
	bool process_scheduler_jobs();
	bool process_terrain_mesh_completion(
		const WtTerrainMeshCompletion &completion
	);
	bool process_mesh_completions();
	bool process_visual_readiness_repairs();
	bool publish_delta(const WtDesiredSetDelta &delta);
	bool push_publication(WtReadOnlyPublication publication);
	void queue_transition_remeshes(
		const std::vector<WtDesiredChunk> &chunks
	);
	void remember_edit_lod_retention_zones(
		const WtEditTransaction &transaction
	);
	std::size_t append_edit_lod_retention_viewers(
		const std::vector<WtLodPlannerViewer> &real_viewers,
		std::vector<WtLodPlannerViewer> &planning_viewers,
		std::uint32_t maximum_refinement_radius_chunks,
		std::size_t maximum_retention_viewers
	) const;
	void notify_work() noexcept;
	void set_failure(WtReadOnlyRuntimeStatus status) noexcept;

	WtRuntimeConfig config_;
	WtAsyncStorageService &storage_;
	WtEditJournalStore *edit_journal_store_ = nullptr;
	std::uint64_t initial_world_revision_ = 0;
	std::atomic<std::uint64_t> world_revision_{ 0 };
	bool valid_ = false;
	std::atomic<bool> stop_requested_{ false };
	std::atomic<WtReadOnlyRuntimeStatus> last_status_{
		WtReadOnlyRuntimeStatus::Ok
	};

	mutable std::mutex input_mutex_;
	std::vector<ViewerEvent> viewer_events_;
	std::size_t viewer_event_capacity_ = 0;
	std::vector<WorldOperation> world_operations_;
	std::size_t world_operation_capacity_ = 0;
	std::uint64_t next_request_id_ = 0;

	mutable std::mutex publication_mutex_;
	std::condition_variable publication_space_available_;
	std::vector<WtReadOnlyPublication> publication_slots_;
	std::size_t publication_head_ = 0;
	std::size_t publication_count_ = 0;

	mutable std::mutex wake_mutex_;
	std::condition_variable wake_condition_;
	std::uint64_t wake_sequence_ = 0;

	std::unique_ptr<WtMultiViewerDesiredSet> desired_;
	std::unique_ptr<WtBalancedLodPlanner> lod_planner_;
	std::vector<WtLodPlannerViewer> planner_viewers_;
	std::vector<EditLodRetentionZone> edit_lod_retention_zones_;
	std::vector<WtDesiredChunk> pending_transition_remeshes_;
	std::uint64_t next_edit_lod_retention_revision_ = 1;
	std::uint64_t next_edit_lod_retention_viewer_id_ = 1;
	WtBalancedLodPlan current_plan_;
	std::uint64_t plan_revision_ = 0;
	std::unique_ptr<WtStreamScheduler> scheduler_;
	std::unique_ptr<WtChunkApplicationService> application_;
	std::unique_ptr<WtStoragePageCache> page_cache_;
	std::unique_ptr<WtChunkResourceCache> resource_cache_;
	std::unique_ptr<WtDesiredSetRuntimeService> desired_runtime_;
	std::unique_ptr<WtEditSpatialIndex> edit_spatial_index_;
	std::unique_ptr<WtEditRuntimeReplacementService> edit_replacement_;
	std::unique_ptr<WtPageMeshingRuntimeService> page_runtime_;
	std::unique_ptr<WtChunkMesher> mesher_;
	std::unique_ptr<WtChunkMeshingScratch> meshing_scratch_;
	mutable std::mutex metrics_mutex_;
	WtReadOnlyRuntimeMetrics metrics_;
};

const char *wt_read_only_runtime_status_message(
	WtReadOnlyRuntimeStatus status
) noexcept;

const char *wt_read_only_edit_status_message(
	WtReadOnlyEditStatus status
) noexcept;

} // namespace world_transvoxel
