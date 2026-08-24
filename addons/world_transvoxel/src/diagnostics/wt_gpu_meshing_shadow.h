#pragma once

#include "diagnostics/wt_gpu_meshing_differential_backend.h"
#include "streaming/wt_stream_scheduler.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace world_transvoxel {

struct WtChunkMeshResult;
struct WtChunkPage;

struct WtGpuMeshingShadowPage {
	WtChunkKey key;
	std::shared_ptr<const WtChunkPage> page;
};

enum class WtGpuMeshingShadowSurface : std::uint8_t {
	Terrain = 0,
	StaticWater = 1,
};

struct WtGpuMeshingShadowCapture {
	WtChunkJob job;
	std::uint8_t transition_mask = 0;
	std::uint8_t cached_transition_mask = 0;
	WtGpuMeshingShadowSurface surface = WtGpuMeshingShadowSurface::Terrain;
	std::vector<WtRecordedMeshingCell> records;
	std::vector<WtGpuMeshingShadowPage> retained_pages;
	std::shared_ptr<const WtChunkMeshResult> authority_terrain_mesh;
	std::shared_ptr<const WtChunkMeshResult> authority_water_mesh;
};

struct WtGpuMeshingShadowRequest : WtGpuMeshingShadowCapture {
	std::uint64_t request_id = 0;
};

struct WtGpuMeshingShadowIdentity {
	WtChunkKey key;
	WtGenerationToken generation;
	std::uint64_t source_revision = 0;
	std::uint64_t world_revision = 0;
	std::uint8_t transition_mask = 0;
	WtGpuMeshingShadowSurface surface = WtGpuMeshingShadowSurface::Terrain;
};

enum class WtGpuMeshingShadowCompletionStatus : std::uint8_t {
	Matched,
	Mismatched,
	Stale,
	UnknownRequest,
	IdentityMismatch,
};

struct WtGpuMeshingShadowCompletion {
	WtGpuMeshingShadowCompletionStatus status =
		WtGpuMeshingShadowCompletionStatus::UnknownRequest;
	std::uint64_t request_id = 0;
	std::string error;
	WtGpuMeshingShadowRequest retained_request;
	bool has_retained_request = false;
};

struct WtGpuMeshingShadowMetrics {
	bool enabled = false;
	std::size_t capacity = 0;
	std::size_t queued_requests = 0;
	std::size_t in_flight_requests = 0;
	std::uint64_t captured_requests = 0;
	std::uint64_t capacity_rejections = 0;
	std::uint64_t superseded_queued_requests = 0;
	std::uint64_t matched_results = 0;
	std::uint64_t mismatched_results = 0;
	std::uint64_t stale_results = 0;
	std::uint64_t unknown_results = 0;
	std::uint64_t identity_mismatches = 0;
};

class WtGpuMeshingShadowQueue {
public:
	bool begin(std::size_t capacity, bool retain_publication_authority = false);
	void end();
	bool enabled() const noexcept;
	bool capture(WtGpuMeshingShadowCapture capture);
	bool pop(WtGpuMeshingShadowRequest &request);
	WtGpuMeshingShadowCompletion complete(
		std::uint64_t request_id,
		const WtGpuMeshingShadowIdentity &identity,
		std::uint64_t current_source_revision,
		std::uint64_t current_world_revision,
		bool matched,
		std::string error
	);
	WtGpuMeshingShadowMetrics metrics() const noexcept;

private:
	static bool identity_matches(
		const WtGpuMeshingShadowRequest &request,
		const WtGpuMeshingShadowIdentity &identity
	) noexcept;
	static bool supersedes_queued(
		const WtGpuMeshingShadowCapture &capture,
		const WtGpuMeshingShadowRequest &queued
	) noexcept;
	bool is_latest_locked(const WtGpuMeshingShadowRequest &request) const noexcept;

	mutable std::mutex mutex_;
	bool enabled_ = false;
	bool retain_publication_authority_ = false;
	std::size_t capacity_ = 0;
	std::uint64_t next_request_id_ = 1;
	std::vector<WtGpuMeshingShadowRequest> queued_;
	std::vector<WtGpuMeshingShadowRequest> in_flight_;
	WtGpuMeshingShadowMetrics metrics_;
};

const char *wt_gpu_meshing_shadow_surface_name(
	WtGpuMeshingShadowSurface surface
) noexcept;
const char *wt_gpu_meshing_shadow_completion_status_name(
	WtGpuMeshingShadowCompletionStatus status
) noexcept;

} // namespace world_transvoxel
