#include "services/wt_runtime_config.h"

#include "services/wt_chunk_resource_cache.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_storage_page_cache.h"
#include "streaming/wt_multi_viewer_desired_set.h"
#include "telemetry/wt_runtime_trace.h"

#include <cmath>

namespace world_transvoxel {
namespace {

bool valid_cache_tier(
	std::uint64_t entries,
	std::uint64_t bytes,
	std::uint64_t maximum_entries,
	std::uint64_t maximum_bytes
) noexcept {
	return entries != 0 && entries <= maximum_entries &&
		bytes != 0 && bytes <= maximum_bytes;
}

} // namespace

WtRuntimeConfigStatus wt_validate_runtime_config(
	const WtRuntimeConfig &config
) noexcept {
	if (config.schema != kWtRuntimeConfigSchema) {
		return WtRuntimeConfigStatus::UnsupportedSchema;
	}
	if (config.active_chunk_capacity == 0 ||
		config.active_chunk_capacity > kWtMaximumRuntimeActiveChunks) {
		return WtRuntimeConfigStatus::InvalidActiveChunkCapacity;
	}
	if (config.viewer_capacity == 0 ||
		config.viewer_capacity > kWtMaximumDesiredViewerCount) {
		return WtRuntimeConfigStatus::InvalidViewerCapacity;
	}
	if (config.demand_capacity_per_viewer == 0 ||
		config.demand_capacity_per_viewer > kWtMaximumDesiredChunkCount) {
		return WtRuntimeConfigStatus::InvalidDemandCapacity;
	}
	if (config.viewer_capacity >
		kWtMaximumDesiredChunkCount / config.demand_capacity_per_viewer) {
		return WtRuntimeConfigStatus::InvalidTotalDemandCapacity;
	}
	if (config.storage_request_capacity == 0 ||
		config.storage_request_capacity > kWtMaximumStorageQueueCapacity ||
		config.storage_completion_capacity == 0 ||
		config.storage_completion_capacity > kWtMaximumStorageQueueCapacity) {
		return WtRuntimeConfigStatus::InvalidStorageQueueCapacity;
	}
	if (!valid_cache_tier(
			config.encoded_page_entry_capacity,
			config.encoded_page_byte_capacity,
			kWtMaximumStorageCacheEntries,
			kWtMaximumStorageCacheBytes
		) || !valid_cache_tier(
			config.decoded_page_entry_capacity,
			config.decoded_page_byte_capacity,
			kWtMaximumStorageCacheEntries,
			kWtMaximumStorageCacheBytes
		)) {
		return WtRuntimeConfigStatus::InvalidStorageCacheCapacity;
	}
	if (!valid_cache_tier(
			config.mesh_entry_capacity,
			config.mesh_byte_capacity,
			kWtMaximumResourceCacheEntries,
			kWtMaximumResourceCacheBytes
		) || !valid_cache_tier(
			config.render_entry_capacity,
			config.render_byte_capacity,
			kWtMaximumResourceCacheEntries,
			kWtMaximumResourceCacheBytes
		) || !valid_cache_tier(
			config.collision_entry_capacity,
			config.collision_byte_capacity,
			kWtMaximumResourceCacheEntries,
			kWtMaximumResourceCacheBytes
		)) {
		return WtRuntimeConfigStatus::InvalidResourceCacheCapacity;
	}
	if (config.trace_event_capacity == 0 ||
		config.trace_event_capacity > kWtMaximumRuntimeTraceEvents) {
		return WtRuntimeConfigStatus::InvalidTraceCapacity;
	}
	if (config.render_apply_budget > kWtMaximumRuntimeApplyBudget ||
		config.collision_apply_budget > kWtMaximumRuntimeApplyBudget) {
		return WtRuntimeConfigStatus::InvalidApplyBudget;
	}
	if (!std::isfinite(config.collision_activation_distance) ||
		!std::isfinite(config.collision_deactivation_distance) ||
		config.collision_activation_distance < 0.0 ||
		config.collision_deactivation_distance <
			config.collision_activation_distance) {
		return WtRuntimeConfigStatus::InvalidCollisionDistance;
	}
	return WtRuntimeConfigStatus::Ok;
}

const char *wt_runtime_config_status_message(
	WtRuntimeConfigStatus status
) noexcept {
	switch (status) {
		case WtRuntimeConfigStatus::Ok: return "ok";
		case WtRuntimeConfigStatus::UnsupportedSchema:
			return "unsupported configuration schema";
		case WtRuntimeConfigStatus::InvalidActiveChunkCapacity:
			return "active chunk capacity must be between 1 and 65536";
		case WtRuntimeConfigStatus::InvalidViewerCapacity:
			return "viewer capacity must be between 1 and 1024";
		case WtRuntimeConfigStatus::InvalidDemandCapacity:
			return "demand capacity per viewer must be between 1 and 65536";
		case WtRuntimeConfigStatus::InvalidTotalDemandCapacity:
			return "viewer and demand capacities exceed 65536 total demands";
		case WtRuntimeConfigStatus::InvalidStorageQueueCapacity:
			return "storage queue capacities must be between 1 and 65536";
		case WtRuntimeConfigStatus::InvalidStorageCacheCapacity:
			return "storage cache capacities exceed schema limits";
		case WtRuntimeConfigStatus::InvalidResourceCacheCapacity:
			return "resource cache capacities exceed schema limits";
		case WtRuntimeConfigStatus::InvalidTraceCapacity:
			return "trace event capacity must be between 1 and 262144";
		case WtRuntimeConfigStatus::InvalidApplyBudget:
			return "application budgets must be between 0 and 128";
		case WtRuntimeConfigStatus::InvalidCollisionDistance:
			return "collision distances must be finite, nonnegative, and ordered";
	}
	return "unknown configuration status";
}

} // namespace world_transvoxel
