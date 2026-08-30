#pragma once

#include "diagnostics/wt_gpu_meshing_differential_backend.h"
#include "streaming/wt_stream_scheduler.h"

#include <cstddef>
#include <cstdint>
#include <functional>
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

enum class WtGpuMeshingCaptureStage : std::uint8_t {
	PostMeshAuthority = 0,
	PreMeshField = 1,
};

struct WtGpuMeshingShadowCapture {
	WtChunkJob job;
	std::uint8_t transition_mask = 0;
	std::uint8_t cached_transition_mask = 0;
	WtGpuMeshingShadowSurface surface = WtGpuMeshingShadowSurface::Terrain;
	WtGpuMeshingCaptureStage capture_stage =
		WtGpuMeshingCaptureStage::PostMeshAuthority;
	bool static_water_surface_expected = false;
	bool cpu_visual_mesh_omitted = false;
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
	bool has_oldest_in_flight_request = false;
	std::uint64_t oldest_in_flight_request_id = 0;
	WtGpuMeshingShadowIdentity oldest_in_flight_identity;
	std::uint64_t captured_requests = 0;
	std::uint64_t capacity_rejections = 0;
	std::uint64_t superseded_queued_requests = 0;
	std::size_t reserved_capture_slots = 0;
	std::uint64_t capture_reservation_attempts = 0;
	std::uint64_t capture_reservation_rejections = 0;
	std::uint64_t reserved_captures = 0;
	std::uint64_t reserved_capture_failures = 0;
	std::uint64_t released_capture_slots = 0;
	std::uint64_t pre_mesh_field_captures = 0;
	std::uint64_t cpu_visual_mesh_omitted_captures = 0;
	std::uint64_t priority_dequeues = 0;
	std::uint64_t dequeue_superseded_requests = 0;
	std::uint64_t matched_results = 0;
	std::uint64_t mismatched_results = 0;
	std::uint64_t stale_results = 0;
	std::uint64_t unknown_results = 0;
	std::uint64_t identity_mismatches = 0;
	std::uint64_t resident_ready_results = 0;
	std::uint64_t resident_rejected_results = 0;
	std::uint64_t resident_stale_results = 0;
};

enum class WtGpuMeshingResidentValidationStatus : std::uint8_t {
	Ready,
	Rejected,
	Stale,
	UnknownRequest,
	IdentityMismatch,
};

struct WtGpuMeshingResidentValidation {
	WtGpuMeshingResidentValidationStatus status =
		WtGpuMeshingResidentValidationStatus::UnknownRequest;
	std::uint64_t request_id = 0;
	std::string error;
};

class WtGpuMeshingShadowQueue {
public:
	bool begin(
		std::size_t capacity,
		bool retain_publication_authority = false,
		WtGpuMeshingCaptureStage capture_stage =
			WtGpuMeshingCaptureStage::PostMeshAuthority
	);
	void end();
	bool enabled() const noexcept;
	bool captures_pre_mesh_field() const noexcept;
	void set_capacity_available_notifier(std::function<void()> notifier);
	std::uint64_t reserve_capture_slots(const WtChunkJob &job);
	bool capture_reserved(
		std::uint64_t reservation_id,
		WtGpuMeshingShadowCapture capture
	);
	void release_capture_slots(std::uint64_t reservation_id) noexcept;
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
	WtGpuMeshingResidentValidation validate_resident(
		std::uint64_t request_id,
		const WtGpuMeshingShadowIdentity &identity,
		std::uint64_t current_source_revision
	);
	WtGpuMeshingResidentValidation reject_resident(
		std::uint64_t request_id,
		const WtGpuMeshingShadowIdentity &identity,
		std::string error
	);
	WtGpuMeshingShadowMetrics metrics() const noexcept;

private:
	struct CaptureReservation {
		std::uint64_t id = 0;
		std::size_t remaining_slots = 0;
		WtChunkJob job;
	};

	static bool identity_matches(
		const WtGpuMeshingShadowRequest &request,
		const WtGpuMeshingShadowIdentity &identity
	) noexcept;
	static bool supersedes_queued(
		const WtGpuMeshingShadowCapture &capture,
		const WtGpuMeshingShadowRequest &queued
	) noexcept;
	static bool same_job_version(
		const WtChunkJob &left,
		const WtChunkJob &right
	) noexcept;
	static bool job_supersedes(
		const WtChunkJob &incoming,
		const WtChunkJob &queued
	) noexcept;
	static bool job_version_supersedes(
		const WtChunkJob &incoming,
		const WtChunkJob &queued
	) noexcept;
	static bool job_precedes(
		const WtChunkJob &left,
		const WtChunkJob &right
	) noexcept;
	bool is_latest_locked(const WtGpuMeshingShadowRequest &request) const noexcept;
	void notify_capacity_available() const;

	mutable std::mutex mutex_;
	mutable std::mutex notifier_mutex_;
	std::function<void()> capacity_available_notifier_;
	bool enabled_ = false;
	bool retain_publication_authority_ = false;
	WtGpuMeshingCaptureStage capture_stage_ =
		WtGpuMeshingCaptureStage::PostMeshAuthority;
	std::size_t capacity_ = 0;
	std::uint64_t next_request_id_ = 1;
	std::uint64_t next_reservation_id_ = 1;
	std::vector<WtGpuMeshingShadowRequest> queued_;
	std::vector<WtGpuMeshingShadowRequest> in_flight_;
	std::vector<CaptureReservation> capture_reservations_;
	WtGpuMeshingShadowMetrics metrics_;
};

const char *wt_gpu_meshing_shadow_surface_name(
	WtGpuMeshingShadowSurface surface
) noexcept;
const char *wt_gpu_meshing_capture_stage_name(
	WtGpuMeshingCaptureStage stage
) noexcept;
const char *wt_gpu_meshing_shadow_completion_status_name(
	WtGpuMeshingShadowCompletionStatus status
) noexcept;
const char *wt_gpu_meshing_resident_validation_status_name(
	WtGpuMeshingResidentValidationStatus status
) noexcept;

} // namespace world_transvoxel
