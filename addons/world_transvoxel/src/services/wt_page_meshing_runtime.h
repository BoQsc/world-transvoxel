#pragma once

#include "diagnostics/wt_gpu_meshing_shadow.h"
#include "meshing/wt_chunk_mesher.h"
#include "services/wt_page_meshing_runtime_owner.h"
#include "streaming/wt_stream_scheduler.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace world_transvoxel {

class WtAsyncStorageService;
class WtEditJournal;
struct WtChunkPage;
class WtStoragePageCache;
struct WtPageLoadCompletion;

constexpr std::size_t kWtMaximumPageMeshingRuntimeRecords = 65536;
constexpr std::size_t kWtMaximumPageMeshingDependencies = 25;

enum class WtPageMeshingRuntimePhase : std::uint8_t {
	Loading,
	SampleReady,
	SampleFailedReady,
	AwaitingMesh,
	Meshing,
	MeshReady,
	MeshFailedReady,
	Ready,
};

enum class WtPageMeshingRuntimeStatus : std::uint8_t {
	Ok,
	InvalidConfiguration,
	InvalidJob,
	InvalidTransitionMask,
	AlreadyPending,
	RecordCapacityExceeded,
	DependencyCapacityExceeded,
	StorageRequestFailure,
	CompletionNotOwned,
	StaleCompletion,
	CacheFailure,
	SchedulerBackpressure,
	NotReady,
	MeshingFailure,
	EditReplayFailure,
	SurfaceShiftFailure,
	NotFound,
	TerrainMeshReadyCallbackFailure,
};

struct WtPageMeshingRuntimeRecordSnapshot {
	WtChunkKey key;
	WtGenerationToken generation;
	std::uint64_t source_revision = 0;
	std::uint64_t world_revision = 0;
	std::int32_t priority = 0;
	std::uint8_t transition_mask = 0;
	std::uint8_t cached_transition_mask = 0;
	WtPageMeshingRuntimePhase phase = WtPageMeshingRuntimePhase::Loading;
	std::size_t dependency_count = 0;
	std::size_t pinned_page_count = 0;
};

struct WtPageMeshingInvalidation {
	WtChunkKey key;
	WtGenerationToken generation;
};

struct WtPageMeshCompletion {
	WtChunkKey key;
	WtGenerationToken generation;
	std::shared_ptr<const WtChunkMeshResult> mesh;
	std::shared_ptr<const WtChunkMeshResult> water_mesh;
	bool gpu_resident_visual_only = false;
};

struct WtTerrainMeshCompletion {
	WtChunkKey key;
	WtGenerationToken generation;
	std::shared_ptr<const WtChunkMeshResult> mesh;
};

using WtTerrainMeshReadyCallback =
	std::function<bool(const WtTerrainMeshCompletion &)>;

struct WtMeshExecutionEvent {
	WtChunkJob job;
	std::uint8_t transition_mask = 0;
	bool started = false;
	std::uint64_t duration_ns = 0;
	WtPageMeshingRuntimeStatus status = WtPageMeshingRuntimeStatus::Ok;
};

using WtMeshExecutionCallback =
	std::function<void(const WtMeshExecutionEvent &)>;
using WtMeshCellCaptureCallback =
	std::function<void(WtGpuMeshingShadowCapture)>;

struct WtPageMeshingRuntimeMetrics {
	std::uint64_t sample_jobs = 0;
	std::uint64_t mesh_jobs = 0;
	std::uint64_t dependency_requests = 0;
	std::uint64_t dependency_reprioritizations = 0;
	std::uint64_t dependency_cache_hits = 0;
	std::uint64_t dependency_cache_misses = 0;
	std::uint64_t accepted_storage_completions = 0;
	std::uint64_t stale_storage_completions = 0;
	std::uint64_t storage_failures = 0;
	std::uint64_t cache_failures = 0;
	std::uint64_t sample_successes = 0;
	std::uint64_t sample_failures = 0;
	std::uint64_t mesh_successes = 0;
	std::uint64_t mesh_failures = 0;
	std::uint64_t gpu_resident_visual_only_completions = 0;
	std::uint64_t surface_shift_rebuilds = 0;
	std::uint64_t surface_shift_failures = 0;
	std::uint64_t scheduler_backpressure = 0;
	std::uint64_t cancellations = 0;
	std::uint64_t invalidated_records = 0;
	std::uint64_t discarded_mesh_completions = 0;
	std::uint64_t mesh_prepare_time_ns_last = 0;
	std::uint64_t mesh_prepare_time_ns_total = 0;
	std::uint64_t mesh_prepare_time_ns_maximum = 0;
	std::uint64_t mesh_completion_time_ns_last = 0;
	std::uint64_t mesh_completion_time_ns_total = 0;
	std::uint64_t mesh_completion_time_ns_maximum = 0;
	std::uint64_t mesh_worker_count = 0;
	std::uint64_t mesh_worker_accepted_jobs = 0;
	std::uint64_t mesh_worker_started_jobs = 0;
	std::uint64_t mesh_worker_completed_jobs = 0;
	std::uint64_t mesh_worker_queue_rejections = 0;
	std::uint64_t mesh_worker_cancelled_queued_jobs = 0;
	std::uint64_t mesh_worker_reprioritized_queued_jobs = 0;
	std::uint64_t mesh_worker_queued_jobs = 0;
	std::uint64_t mesh_worker_queued_completions = 0;
	std::uint64_t mesh_worker_active_jobs = 0;
	std::uint64_t mesh_worker_maximum_active_jobs = 0;
	std::uint64_t mesh_worker_queue_wait_ns_last = 0;
	std::uint64_t mesh_worker_queue_wait_ns_total = 0;
	std::uint64_t mesh_worker_queue_wait_ns_maximum = 0;
	std::uint64_t mesh_worker_execute_time_ns_last = 0;
	std::uint64_t mesh_worker_execute_time_ns_total = 0;
	std::uint64_t mesh_worker_execute_time_ns_maximum = 0;
	std::size_t maximum_pinned_pages = 0;
	std::uint64_t loading_records = 0;
	std::uint64_t sample_ready_records = 0;
	std::uint64_t awaiting_mesh_records = 0;
	std::uint64_t meshing_records = 0;
	std::uint64_t mesh_ready_records = 0;
	std::uint64_t ready_records = 0;
	std::uint64_t unresolved_dependencies = 0;
	std::uint64_t pending_dependency_requests = 0;
	std::uint64_t pinned_pages = 0;
	std::int64_t last_failure_key_x = 0;
	std::int64_t last_failure_key_y = 0;
	std::int64_t last_failure_key_z = 0;
	std::uint64_t last_failure_key_lod = 0;
};

class WtPageMeshingRuntimeService final : public WtPageMeshingRuntimeOwner {
public:
	explicit WtPageMeshingRuntimeService(
		std::size_t record_capacity,
		std::size_t meshing_worker_count = 0
	);
	~WtPageMeshingRuntimeService();

	bool valid() const noexcept;
	WtPageMeshingRuntimeStatus begin_sample_job(
		const WtChunkJob &job,
		std::uint8_t transition_mask,
		WtAsyncStorageService &storage,
		WtStoragePageCache &cache,
		WtStreamScheduler &scheduler
	);
	WtPageMeshingRuntimeStatus begin_sample_job(
		const WtChunkJob &job,
		std::uint8_t transition_mask,
		std::uint8_t requested_cached_transition_mask,
		WtAsyncStorageService &storage,
		WtStoragePageCache &cache,
		WtStreamScheduler &scheduler
	);
	WtPageMeshingRuntimeStatus accept_storage_completion(
		const WtPageLoadCompletion &completion,
		WtStoragePageCache &cache,
		WtStreamScheduler &scheduler
	);
	WtPageMeshingRuntimeStatus execute_mesh_job(
		const WtChunkJob &job,
		const WtChunkMesher &mesher,
		WtChunkMeshingScratch &scratch,
		WtStreamScheduler &scheduler,
		const WtEditJournal *edit_journal = nullptr,
		std::uint64_t initial_world_revision = 0,
		WtAsyncStorageService *authoritative_storage = nullptr,
		const WtTerrainMeshReadyCallback &terrain_mesh_ready = {},
		bool visual_required = true,
		const WtMeshCellCaptureCallback &cell_capture_callback = {},
		bool pre_mesh_field_capture = false,
		bool collision_required = false
	);
	WtPageMeshingRuntimeStatus dispatch_mesh_job(
		const WtChunkJob &job,
		WtStreamScheduler &scheduler,
		const WtEditJournal *edit_journal = nullptr,
		std::uint64_t initial_world_revision = 0,
		WtAsyncStorageService *authoritative_storage = nullptr,
		const WtTerrainMeshReadyCallback &terrain_mesh_ready = {},
		bool visual_required = true,
		const WtMeshExecutionCallback &execution_callback = {},
		const WtMeshCellCaptureCallback &cell_capture_callback = {},
		bool pre_mesh_field_capture = false,
		bool collision_required = false
	);
	WtPageMeshingRuntimeStatus process_async_mesh_completions(
		WtStreamScheduler &scheduler,
		std::size_t maximum_count,
		std::size_t &processed
	);
	void set_mesh_completion_notifier(std::function<void()> notifier);
	bool asynchronous_meshing_enabled() const noexcept;
	bool asynchronous_mesh_admission_available() const noexcept;

	std::size_t flush_scheduler_results(
		WtStreamScheduler &scheduler
	);
	std::size_t resume_loading_records(
		WtAsyncStorageService &storage,
		WtStoragePageCache &cache,
		WtStreamScheduler &scheduler,
		std::size_t maximum_records
	);
	bool pop_mesh_completion(WtPageMeshCompletion &completion);

	WtPageMeshingRuntimeStatus cancel_generation(
		const WtChunkKey &key,
		WtGenerationToken generation
	);
	WtPageMeshingRuntimeStatus release_chunk(const WtChunkKey &key);
	WtPageMeshingRuntimeStatus reprioritize(
		const WtChunkKey &key,
		WtGenerationToken generation,
		std::int32_t priority
	);
	WtPageMeshingRuntimeStatus invalidate_dependency(
		const WtChunkKey &dependency,
		std::vector<WtPageMeshingInvalidation> &invalidated
	);
	WtPageMeshingRuntimeOwnerStatus cancel_owned_generation(
		const WtChunkKey &key,
		WtGenerationToken generation
	) override;
	WtPageMeshingRuntimeOwnerStatus release_owned_chunk(
		const WtChunkKey &key
	) override;
	WtPageMeshingRuntimeOwnerStatus reprioritize_owned_chunk(
		const WtChunkKey &key,
		WtGenerationToken generation,
		std::int32_t priority
	) override;

	std::vector<WtPageMeshingRuntimeRecordSnapshot>
	get_records() const;
	bool copy_record(
		const WtChunkKey &key,
		WtGenerationToken generation,
		WtPageMeshingRuntimeRecordSnapshot &output
	) const noexcept;
	std::size_t record_count() const noexcept;
	std::size_t record_capacity() const noexcept;
	std::size_t pinned_page_count() const noexcept;
	WtPageMeshingRuntimeMetrics get_metrics() const noexcept;

private:
	struct PreparedDependency;
	struct PreparedMeshJob;
	struct PreparedMeshCompletion;
	struct AsyncState;
	struct Dependency {
		WtChunkKey key;
		std::shared_ptr<const WtChunkPage> page;
		bool request_pending = false;
		std::int32_t requested_priority = 0;
	};

	struct Record {
		WtChunkKey key;
		WtGenerationToken generation;
		std::uint64_t source_revision = 0;
		std::uint64_t world_revision = 0;
		std::int32_t priority = 0;
		std::uint8_t transition_mask = 0;
		std::uint8_t cached_transition_mask = 0;
		WtPageMeshingRuntimePhase phase =
			WtPageMeshingRuntimePhase::Loading;
		std::vector<Dependency> dependencies;
		std::shared_ptr<const WtChunkMeshResult> mesh;
		std::shared_ptr<const WtChunkMeshResult> water_mesh;
		bool gpu_resident_visual_only = false;
	};
	struct LoadingRetryCandidate {
		WtChunkKey key;
		std::int32_t priority = 0;
	};

	std::vector<Record>::iterator find_record(
		const WtChunkKey &key
	) noexcept;
	std::vector<Record>::const_iterator find_record(
		const WtChunkKey &key
	) const noexcept;
	WtPageMeshingRuntimeStatus resolve_dependency(
		std::size_t record_index,
		std::size_t dependency_index,
		WtAsyncStorageService &storage,
		WtStoragePageCache &cache,
		WtStreamScheduler &scheduler,
		bool &made_progress
	);
	WtPageMeshingRuntimeStatus resolve_loading_record(
		std::size_t record_index,
		WtAsyncStorageService &storage,
		WtStoragePageCache &cache,
		WtStreamScheduler &scheduler,
		bool &made_progress
	);
	WtPageMeshingRuntimeStatus submit_pending_result(
		std::size_t record_index,
		WtStreamScheduler &scheduler
	);
	WtPageMeshingRuntimeStatus mark_sample_failure(
		std::size_t record_index,
		WtStreamScheduler &scheduler
	);
	WtPageMeshingRuntimeStatus prepare_mesh_job(
		const WtChunkJob &job,
		WtStreamScheduler &scheduler,
		const WtEditJournal *edit_journal,
		std::uint64_t initial_world_revision,
		WtAsyncStorageService *authoritative_storage,
		const WtTerrainMeshReadyCallback &terrain_mesh_ready,
		bool visual_required,
		const WtMeshExecutionCallback &execution_callback,
		const WtMeshCellCaptureCallback &cell_capture_callback,
		bool pre_mesh_field_capture,
		bool collision_required,
		PreparedMeshJob &prepared
	);
	static PreparedMeshCompletion execute_prepared_mesh_job(
		PreparedMeshJob prepared,
		const WtChunkMesher &mesher,
		WtChunkMeshingScratch &scratch
	);
	WtPageMeshingRuntimeStatus accept_prepared_mesh_completion(
		PreparedMeshCompletion completion,
		WtStreamScheduler &scheduler,
		bool synchronous_compatibility = false
	);
	void merge_async_metrics(WtPageMeshingRuntimeMetrics &metrics) const noexcept;
	std::size_t cancel_async_work(
		const WtChunkKey &key,
		WtGenerationToken generation
	) noexcept;
	bool reprioritize_async_work(
		const WtChunkKey &key,
		WtGenerationToken generation,
		std::int32_t priority
	) noexcept;
	void update_maximum_pins() noexcept;

	std::size_t record_capacity_ = 0;
	bool valid_ = false;
	std::vector<Record> records_;
	std::vector<LoadingRetryCandidate> loading_retry_candidates_;
	WtPageMeshingRuntimeMetrics metrics_;
	WtChunkMeshingScratch preparation_scratch_;
	std::unique_ptr<AsyncState> async_;
};

} // namespace world_transvoxel
