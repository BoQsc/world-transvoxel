#pragma once

#include "streaming/wt_multi_viewer_desired_set.h"

#include <cstddef>
#include <cstdint>

namespace world_transvoxel {

class WtChunkApplicationService;
class WtChunkResourceCache;
class WtPageMeshingRuntimeOwner;
class WtStoragePageCache;
class WtStreamScheduler;

enum class WtDesiredSetRuntimeStatus : std::uint8_t {
	Ok,
	InvalidConfiguration,
	InvalidDelta,
	ChangeCapacityExceeded,
	RuntimeStateMismatch,
	RecordCapacityExceeded,
	JobQueueCapacityExceeded,
	SchedulerFailure,
	ApplicationFailure,
	PageMeshingRuntimeFailure,
};

struct WtDesiredSetRuntimeMetrics {
	std::uint64_t delta_attempts = 0;
	std::uint64_t applied_deltas = 0;
	std::uint64_t empty_deltas = 0;
	std::uint64_t added_chunks = 0;
	std::uint64_t removed_chunks = 0;
	std::uint64_t updated_chunks = 0;
	std::uint64_t evicted_page_entries = 0;
	std::uint64_t evicted_resource_entries = 0;
	std::uint64_t invalid_deltas = 0;
	std::uint64_t capacity_rejections = 0;
	std::uint64_t state_rejections = 0;
	std::uint64_t scheduler_failures = 0;
	std::uint64_t application_failures = 0;
	std::uint64_t page_meshing_runtime_failures = 0;
	std::uint64_t released_page_meshing_records = 0;
	std::uint64_t reprioritized_page_meshing_records = 0;
};

class WtDesiredSetRuntimeService {
public:
	explicit WtDesiredSetRuntimeService(std::size_t change_capacity);

	bool valid() const noexcept;
	WtDesiredSetRuntimeStatus apply_delta(
		const WtDesiredSetDelta &delta,
		std::uint64_t source_revision,
		std::uint64_t world_revision,
		WtStreamScheduler &scheduler,
		WtStoragePageCache &page_cache,
		WtChunkResourceCache &resource_cache,
		WtChunkApplicationService &application,
		WtPageMeshingRuntimeOwner *page_meshing_runtime
	);

	std::size_t change_capacity() const noexcept;
	WtDesiredSetRuntimeMetrics get_metrics() const noexcept;

private:
	bool validate_delta(const WtDesiredSetDelta &delta) const noexcept;

	std::size_t change_capacity_ = 0;
	bool valid_ = false;
	WtDesiredSetRuntimeMetrics metrics_;
};

} // namespace world_transvoxel
