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
};

class WtEditRuntimeReplacementService {
public:
	explicit WtEditRuntimeReplacementService(std::size_t replacement_capacity);

	bool valid() const noexcept;
	WtEditRuntimeReplacementStatus prepare_loaded_chunks(
		const WtEditTransaction &transaction,
		const WtEditSpatialIndex &spatial_index,
		const WtStreamScheduler &scheduler,
		const WtChunkApplicationService &application
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
		WtPageMeshingRuntimeOwner *page_meshing_runtime
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
		bool collision_required = false;
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
