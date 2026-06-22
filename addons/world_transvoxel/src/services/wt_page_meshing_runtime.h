#pragma once

#include "meshing/wt_chunk_mesher.h"
#include "services/wt_page_meshing_runtime_owner.h"
#include "streaming/wt_stream_scheduler.h"

#include <cstddef>
#include <cstdint>
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
	NotFound,
};

struct WtPageMeshingRuntimeRecordSnapshot {
	WtChunkKey key;
	WtGenerationToken generation;
	std::uint64_t source_revision = 0;
	std::uint64_t world_revision = 0;
	std::int32_t priority = 0;
	std::uint8_t transition_mask = 0;
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
};

struct WtPageMeshingRuntimeMetrics {
	std::uint64_t sample_jobs = 0;
	std::uint64_t mesh_jobs = 0;
	std::uint64_t dependency_requests = 0;
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
	std::uint64_t scheduler_backpressure = 0;
	std::uint64_t cancellations = 0;
	std::uint64_t invalidated_records = 0;
	std::uint64_t discarded_mesh_completions = 0;
	std::size_t maximum_pinned_pages = 0;
};

class WtPageMeshingRuntimeService final : public WtPageMeshingRuntimeOwner {
public:
	explicit WtPageMeshingRuntimeService(
		std::size_t record_capacity
	);

	bool valid() const noexcept;
	WtPageMeshingRuntimeStatus begin_sample_job(
		const WtChunkJob &job,
		std::uint8_t transition_mask,
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
		std::uint64_t initial_world_revision = 0
	);

	std::size_t flush_scheduler_results(
		WtStreamScheduler &scheduler
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
	std::size_t record_count() const noexcept;
	std::size_t record_capacity() const noexcept;
	std::size_t pinned_page_count() const noexcept;
	WtPageMeshingRuntimeMetrics get_metrics() const noexcept;

private:
	struct Dependency {
		WtChunkKey key;
		std::shared_ptr<const WtChunkPage> page;
	};

	struct Record {
		WtChunkKey key;
		WtGenerationToken generation;
		std::uint64_t source_revision = 0;
		std::uint64_t world_revision = 0;
		std::int32_t priority = 0;
		std::uint8_t transition_mask = 0;
		WtPageMeshingRuntimePhase phase =
			WtPageMeshingRuntimePhase::Loading;
		std::vector<Dependency> dependencies;
		std::shared_ptr<const WtChunkMeshResult> mesh;
	};

	std::vector<Record>::iterator find_record(
		const WtChunkKey &key
	) noexcept;
	std::vector<Record>::const_iterator find_record(
		const WtChunkKey &key
	) const noexcept;
	std::vector<Record>::iterator find_completion_owner(
		const WtPageLoadCompletion &completion
	) noexcept;
	WtPageMeshingRuntimeStatus submit_pending_result(
		std::size_t record_index,
		WtStreamScheduler &scheduler
	);
	WtPageMeshingRuntimeStatus mark_sample_failure(
		std::size_t record_index,
		WtStreamScheduler &scheduler
	);
	void update_maximum_pins() noexcept;

	std::size_t record_capacity_ = 0;
	bool valid_ = false;
	std::vector<Record> records_;
	WtPageMeshingRuntimeMetrics metrics_;
};

} // namespace world_transvoxel
