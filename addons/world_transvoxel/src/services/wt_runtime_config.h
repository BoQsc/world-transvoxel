#pragma once

// Cross-service construction limits validated before runtime startup.

#include <cstdint>

namespace world_transvoxel {

constexpr std::uint32_t kWtRuntimeConfigSchema = 1;
constexpr std::uint64_t kWtMaximumRuntimeActiveChunks = 65536;
constexpr std::uint64_t kWtMaximumRuntimeApplyBudget = 128;

struct WtRuntimeConfig {
	std::uint32_t schema = kWtRuntimeConfigSchema;
	std::uint64_t active_chunk_capacity = 256;
	std::uint64_t viewer_capacity = 8;
	std::uint64_t demand_capacity_per_viewer = 4096;
	std::uint64_t lod_refinement_radius_chunks = 0;
	std::uint64_t storage_request_capacity = 256;
	std::uint64_t storage_completion_capacity = 256;
	std::uint64_t encoded_page_entry_capacity = 256;
	std::uint64_t encoded_page_byte_capacity = 64U * 1024U * 1024U;
	std::uint64_t decoded_page_entry_capacity = 128;
	std::uint64_t decoded_page_byte_capacity = 64U * 1024U * 1024U;
	std::uint64_t mesh_entry_capacity = 128;
	std::uint64_t mesh_byte_capacity = 128U * 1024U * 1024U;
	std::uint64_t render_entry_capacity = 128;
	std::uint64_t render_byte_capacity = 128U * 1024U * 1024U;
	std::uint64_t collision_entry_capacity = 64;
	std::uint64_t collision_byte_capacity = 64U * 1024U * 1024U;
	std::uint64_t trace_event_capacity = 65536;
	std::uint64_t render_apply_budget = 4;
	std::uint64_t collision_apply_budget = 2;
	double collision_activation_distance = 96.0;
	double collision_deactivation_distance = 128.0;
};

enum class WtRuntimeConfigStatus : std::uint8_t {
	Ok,
	UnsupportedSchema,
	InvalidActiveChunkCapacity,
	InvalidViewerCapacity,
	InvalidDemandCapacity,
	InvalidLodRefinementRadius,
	InvalidTotalDemandCapacity,
	InvalidStorageQueueCapacity,
	InvalidStorageCacheCapacity,
	InvalidResourceCacheCapacity,
	InvalidTraceCapacity,
	InvalidApplyBudget,
	InvalidCollisionDistance,
};

WtRuntimeConfigStatus wt_validate_runtime_config(
	const WtRuntimeConfig &config
) noexcept;

const char *wt_runtime_config_status_message(
	WtRuntimeConfigStatus status
) noexcept;

} // namespace world_transvoxel
