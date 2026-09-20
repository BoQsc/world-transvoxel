#pragma once

#include "core/wt_chunk_state.h"
#include "editing/wt_edit_spatial_index.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

class WtChunkApplicationService;
class WtChunkResourceCache;
class WtPageMeshingRuntimeOwner;
class WtStoragePageCache;
class WtStreamScheduler;
struct WtDesiredChunk;

constexpr std::size_t kWtMaximumEditRuntimeReplacements = 65536;

enum class WtEditRuntimeReplacementStatus : std::uint8_t {
	Ok,
	InvalidConfiguration,
	SpatialQueryFailed,
	AffectedCapacityExceeded,
	RuntimeStateMismatch,
	SourceRevisionMismatch,
	WorldRevisionMismatch,
	JobQueueCapacityExceeded,
	SchedulerFailure,
	ApplicationFailure,
	PageMeshingRuntimeFailure,
};

struct WtEditRuntimeReplacementRecord {
	WtChunkKey key;
	WtGenerationToken previous_generation;
	WtGenerationToken replacement_generation;
	std::uint64_t source_revision = 0;
	std::uint64_t previous_world_revision = 0;
	std::uint64_t replacement_world_revision = 0;
	std::size_t evicted_page_entries = 0;
	std::size_t evicted_resource_entries = 0;
	bool collision_required = false;
	bool visual_required = true;
	bool independently_publishable = false;
	bool atomic_visual_edit_member = false;
};

struct WtEditRuntimeReplacementMetrics {
	std::uint64_t transaction_attempts = 0;
	std::uint64_t completed_transactions = 0;
	std::uint64_t empty_transactions = 0;
	std::uint64_t queried_chunks = 0;
	std::uint64_t replaced_chunks = 0;
	std::uint64_t evicted_page_entries = 0;
	std::uint64_t evicted_resource_entries = 0;
	std::uint64_t spatial_rejections = 0;
	std::uint64_t capacity_rejections = 0;
	std::uint64_t state_rejections = 0;
	std::uint64_t scheduler_failures = 0;
	std::uint64_t application_failures = 0;
	std::uint64_t page_meshing_runtime_failures = 0;
	std::uint64_t cancelled_page_meshing_generations = 0;
	std::uint64_t exact_delta_chunks = 0;
	std::uint64_t exact_delta_dirty_blocks = 0;
	std::uint64_t maximum_dirty_blocks_per_chunk = 0;
	std::uint64_t active_visual_cohort_chunks = 0;
	std::uint64_t deferred_inactive_visual_chunks = 0;
};

class WtEditRuntimeReplacementService {
public:
	explicit WtEditRuntimeReplacementService(std::size_t replacement_capacity);

	bool valid() const noexcept;
	WtEditRuntimeReplacementStatus prepare_loaded_chunks(
		const WtEditTransaction &transaction,
		const WtEditSpatialIndex &spatial_index,
		const WtStreamScheduler &scheduler,
		const WtChunkApplicationService &application,
		const std::vector<WtDesiredChunk> *desired_chunks = nullptr,
		const std::vector<WtChunkKey> *active_visual_chunks = nullptr
	);
	WtEditRuntimeReplacementStatus apply_prepared(
		const WtEditTransaction &transaction,
		WtStreamScheduler &scheduler,
		WtStoragePageCache &page_cache,
		WtChunkResourceCache &resource_cache,
		WtChunkApplicationService &application,
		WtPageMeshingRuntimeOwner *page_meshing_runtime
	);
	WtEditRuntimeReplacementStatus replace_loaded_chunks(
		const WtEditTransaction &transaction,
		const WtEditSpatialIndex &spatial_index,
		WtStreamScheduler &scheduler,
		WtStoragePageCache &page_cache,
		WtChunkResourceCache &resource_cache,
		WtChunkApplicationService &application,
		WtPageMeshingRuntimeOwner *page_meshing_runtime,
		const std::vector<WtDesiredChunk> *desired_chunks = nullptr,
		const std::vector<WtChunkKey> *active_visual_chunks = nullptr
	);

	std::size_t replacement_capacity() const noexcept;
	const std::vector<WtEditRuntimeReplacementRecord> &
	get_last_replacements() const noexcept;
	WtEditRuntimeReplacementMetrics get_metrics() const noexcept;

private:
	struct PreparedReplacement {
		WtChunkKey key;
		WtGenerationToken previous_generation;
		std::uint64_t source_revision = 0;
		std::uint64_t previous_world_revision = 0;
		std::int32_t priority = 0;
		bool collision_required = false;
		bool visual_required = true;
		bool foreground_interaction = false;
		bool independently_publishable = false;
		bool deferred_inactive_visual = false;
		bool atomic_visual_edit_member = false;
		WtChunkEditDelta edit_delta;
	};

	std::size_t replacement_capacity_ = 0;
	bool valid_ = false;
	std::vector<WtChunkKey> affected_;
	std::vector<PreparedReplacement> prepared_;
	std::vector<WtEditRuntimeReplacementRecord> last_replacements_;
	std::uint64_t prepared_source_revision_ = 0;
	std::uint64_t prepared_base_revision_ = 0;
	std::uint64_t prepared_committed_revision_ = 0;
	bool has_prepared_ = false;
	WtEditRuntimeReplacementMetrics metrics_;
};

} // namespace world_transvoxel
