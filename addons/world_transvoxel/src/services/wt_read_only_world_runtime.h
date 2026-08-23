#pragma once

#include "physics/wt_collision_apply_queue.h"
#include "render/wt_render_apply_queue.h"
#include "services/wt_authoritative_sample_query.h"
#include "services/wt_read_only_world_runtime_metrics.h"
#include "services/wt_runtime_config.h"
#include "storage/wt_world_snapshot_store.h"
#include "streaming/wt_balanced_lod_planner.h"
#include "streaming/wt_foreground_priority.h"
#include "editing/wt_edit_transaction.h"
#include "streaming/wt_multi_viewer_desired_set.h"
#include "telemetry/wt_causal_trace.h"

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
enum class WtDesiredSetRuntimeStatus : std::uint8_t;
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
	InvalidForegroundPriority,
	ForegroundPriorityQueueFull,
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
	PipelineStorageCompletionFailure,
	PipelineSchedulerJobFailure,
	PipelineTerrainMeshCompletionFailure,
	PipelineRenderCompletionFailure,
	PipelineTransitionMaskUpdateFailure,
	PipelineCollisionRebuildFailure,
	PipelineCollisionRepairFailure,
	PublicationFailure,
};

enum class WtReadOnlyPublicationKind : std::uint8_t {
	ExpectChunk,
	SetCollisionRequired,
	SetVisualRequired,
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
	ViewerPlanStarted,
	ViewerPlanCompleted,
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
	bool preserve_collision_ready = false;
	bool visual_required = true;
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
	bool independently_publishable_replacement = false;
};

struct WtVisibilityCoveragePriorityRequest {
	WtChunkKey key;
	WtGenerationToken generation;
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
	WtReadOnlyRuntimeStatus update_collision_viewer(
		const WtViewerSnapshot &snapshot,
		std::uint32_t radius_chunks
	);
	WtReadOnlyRuntimeStatus remove_collision_viewer(
		std::uint64_t viewer_id,
		std::uint64_t revision
	);
	WtReadOnlyRuntimeStatus submit_edit(
		const WtEditTransaction &transaction
	);
	WtReadOnlyRuntimeStatus request_visibility_coverage_priority_batch(
		const std::vector<WtVisibilityCoveragePriorityRequest> &requests
	);
	WtReadOnlyRuntimeStatus update_foreground_priority_lease(
		const WtForegroundPriorityLeaseRequest &request
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
	bool pop_unbudgeted_publication(WtReadOnlyPublication &publication);
	void notify_application_progress() noexcept;

	WtReadOnlyRuntimeStatus last_status() const noexcept;
	WtReadOnlyRuntimeMetrics get_metrics() const noexcept;
	std::uint64_t world_revision() const noexcept;
	bool begin_causal_trace();
	void end_causal_trace();
	WtCausalTraceSnapshot causal_trace_snapshot(
		std::uint64_t first_sequence,
		std::size_t maximum_events
	) const;
	void record_frontend_publication(
		const WtReadOnlyPublication &publication,
		std::int64_t status
	);
	void record_frontend_sink(
		bool collision,
		const WtChunkKey &key,
		WtGenerationToken generation,
		std::uint64_t duration_ns,
		bool applied
	);
	void record_frontend_visibility(
		WtCausalTraceEventKind kind,
		const WtChunkKey *key,
		WtGenerationToken generation,
		std::uint64_t cause_id,
		std::uint64_t auxiliary,
		std::int64_t status
	);

private:
	enum class ViewerEventKind : std::uint8_t {
		Update,
		Remove,
		UpdateCollision,
		RemoveCollision,
	};
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
		VisibilityCoveragePriority,
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
		std::vector<WtVisibilityCoveragePriorityRequest>
			visibility_coverage_priority_requests;
	};
	struct ForegroundPriorityEvent {
		WtForegroundPriorityLeaseRequest request;
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
	struct CollisionViewer {
		WtViewerSnapshot snapshot;
		std::uint32_t radius_chunks = 0;
	};
	struct ReadinessRepairAttempt {
		WtChunkKey key;
		WtGenerationToken generation;
		bool render_republished = false;
	};
	struct ReadinessRepairRemeshAttempt {
		WtChunkKey key;
		WtGenerationToken generation;
	};
	struct CollisionReadinessRepairAttempt {
		WtChunkKey key;
		WtGenerationToken generation;
	};

	bool enqueue_viewer_event(const ViewerEvent &event);
	bool process_viewer_event();
	bool enqueue_foreground_priority_event(
		const ForegroundPriorityEvent &event
	);
	bool process_foreground_priority_event();
	bool enqueue_world_operation(WorldOperation &operation);
	bool has_pending_edit_operation();
	bool process_world_operation_event();
	bool process_edit_operation(const WtEditTransaction &transaction);
	bool process_sample_query_operation(const WorldOperation &operation);
	bool process_sample_batch_query_operation(const WorldOperation &operation);
	bool process_snapshot_operation(const WorldOperation &operation);
	bool process_visibility_coverage_priority_operation(
		const WorldOperation &operation
	);
	bool process_storage_completions();
	bool process_pending_transition_remeshes();
	bool process_scheduler_jobs();
	bool process_async_mesh_completions();
	bool prepare_terrain_collision_payload(
		const WtTerrainMeshCompletion &completion,
		std::shared_ptr<WtCollisionPayload> &collision
	);
	bool process_terrain_mesh_completion(
		const WtTerrainMeshCompletion &completion
	);
	bool process_mesh_completions();
	bool process_collision_readiness_repairs();
	bool process_visual_readiness_repairs();
	bool publish_delta(const WtDesiredSetDelta &delta);
	bool publish_transition_mask_update(
		const WtLodMapEntry &entry,
		const WtDesiredChunk &desired
	);
	bool push_publication(WtReadOnlyPublication publication);
	bool has_publication_backlog();
	static bool is_priority_publication(
		const WtReadOnlyPublication &publication
	) noexcept;
	void queue_transition_remeshes(
		const std::vector<WtDesiredChunk> &chunks
	);
	void queue_readiness_repair_candidate(const WtChunkKey &key);
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
	static WtReadOnlyRuntimeStatus delta_failure_status(
		WtDesiredSetRuntimeStatus status
	) noexcept;
	void set_failure(WtReadOnlyRuntimeStatus status) noexcept;
	void refresh_metrics_snapshot() noexcept;

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
	std::atomic<std::uint64_t> application_progress_sequence_{ 0 };

	mutable std::mutex input_mutex_;
	std::vector<ViewerEvent> viewer_events_;
	std::size_t viewer_event_capacity_ = 0;
	std::vector<ForegroundPriorityEvent> foreground_priority_events_;
	std::size_t foreground_priority_event_capacity_ = 0;
	std::vector<WorldOperation> world_operations_;
	std::size_t world_operation_capacity_ = 0;
	std::uint64_t next_request_id_ = 0;
	std::atomic<bool> pending_edit_operation_{ false };

	mutable std::mutex publication_mutex_;
	std::condition_variable publication_space_available_;
	std::vector<WtReadOnlyPublication> priority_publication_slots_;
	std::size_t priority_publication_head_ = 0;
	std::size_t priority_publication_count_ = 0;
	std::vector<WtReadOnlyPublication> publication_slots_;
	std::size_t publication_head_ = 0;
	std::size_t publication_count_ = 0;
	std::size_t priority_publication_burst_ = 0;

	mutable std::mutex wake_mutex_;
	std::condition_variable wake_condition_;
	std::uint64_t wake_sequence_ = 0;

	std::unique_ptr<WtMultiViewerDesiredSet> desired_;
	WtForegroundPriorityLeaseSet foreground_priority_leases_;
	std::vector<WtViewerChunkDemand> base_demands_;
	std::unique_ptr<WtBalancedLodPlanner> lod_planner_;
	std::vector<WtLodPlannerViewer> planner_viewers_;
	std::vector<CollisionViewer> collision_viewers_;
	std::vector<EditLodRetentionZone> edit_lod_retention_zones_;
	std::vector<WtDesiredChunk> pending_transition_remeshes_;
	std::vector<WtChunkKey> readiness_repair_candidate_keys_;
	std::vector<CollisionReadinessRepairAttempt>
		collision_readiness_repair_attempts_;
	std::vector<ReadinessRepairAttempt> readiness_repair_attempts_;
	std::vector<ReadinessRepairRemeshAttempt> readiness_repair_remesh_attempts_;
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
	WtReadOnlyRuntimeMetrics published_metrics_;
	WtCausalTraceBuffer causal_trace_;
};

const char *wt_read_only_runtime_status_message(
	WtReadOnlyRuntimeStatus status
) noexcept;

const char *wt_read_only_edit_status_message(
	WtReadOnlyEditStatus status
) noexcept;

} // namespace world_transvoxel
