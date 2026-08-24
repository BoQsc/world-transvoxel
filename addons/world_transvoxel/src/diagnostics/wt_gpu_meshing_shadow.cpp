#include "diagnostics/wt_gpu_meshing_shadow.h"

#include <algorithm>
#include <utility>

namespace world_transvoxel {

bool WtGpuMeshingShadowQueue::begin(std::size_t capacity) {
	if (capacity == 0 || capacity > 16) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	enabled_ = true;
	capacity_ = capacity;
	next_request_id_ = 1;
	queued_.clear();
	in_flight_.clear();
	metrics_ = {};
	metrics_.enabled = true;
	metrics_.capacity = capacity;
	return true;
}

void WtGpuMeshingShadowQueue::end() {
	std::lock_guard<std::mutex> lock(mutex_);
	enabled_ = false;
	capacity_ = 0;
	queued_.clear();
	in_flight_.clear();
	metrics_.enabled = false;
	metrics_.capacity = 0;
	metrics_.queued_requests = 0;
	metrics_.in_flight_requests = 0;
}

bool WtGpuMeshingShadowQueue::enabled() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	return enabled_;
}

bool WtGpuMeshingShadowQueue::capture(WtGpuMeshingShadowCapture capture) {
	if (capture.records.empty()) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!enabled_) return false;
	if (queued_.size() + in_flight_.size() >= capacity_) {
		++metrics_.capacity_rejections;
		return false;
	}
	WtGpuMeshingShadowRequest request;
	static_cast<WtGpuMeshingShadowCapture &>(request) = std::move(capture);
	request.request_id = next_request_id_++;
	queued_.push_back(std::move(request));
	++metrics_.captured_requests;
	metrics_.queued_requests = queued_.size();
	return true;
}

bool WtGpuMeshingShadowQueue::pop(WtGpuMeshingShadowRequest &request) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!enabled_ || queued_.empty()) return false;
	request = std::move(queued_.front());
	queued_.erase(queued_.begin());
	WtGpuMeshingShadowRequest tracked_request;
	tracked_request.job = request.job;
	tracked_request.transition_mask = request.transition_mask;
	tracked_request.cached_transition_mask = request.cached_transition_mask;
	tracked_request.surface = request.surface;
	tracked_request.request_id = request.request_id;
	in_flight_.push_back(std::move(tracked_request));
	metrics_.queued_requests = queued_.size();
	metrics_.in_flight_requests = in_flight_.size();
	return true;
}

WtGpuMeshingShadowCompletion WtGpuMeshingShadowQueue::complete(
	std::uint64_t request_id,
	const WtGpuMeshingShadowIdentity &identity,
	std::uint64_t current_source_revision,
	std::uint64_t current_world_revision,
	bool matched,
	std::string error
) {
	std::lock_guard<std::mutex> lock(mutex_);
	WtGpuMeshingShadowCompletion completion;
	completion.request_id = request_id;
	const auto found = std::find_if(
		in_flight_.begin(), in_flight_.end(),
		[request_id](const WtGpuMeshingShadowRequest &request) {
			return request.request_id == request_id;
		}
	);
	if (found == in_flight_.end()) {
		++metrics_.unknown_results;
		completion.error = "GPU shadow request is not in flight";
		return completion;
	}
	WtGpuMeshingShadowRequest request = std::move(*found);
	in_flight_.erase(found);
	metrics_.in_flight_requests = in_flight_.size();
	if (!identity_matches(request, identity)) {
		completion.status = WtGpuMeshingShadowCompletionStatus::IdentityMismatch;
		completion.error = "GPU shadow result identity does not match its request";
		++metrics_.identity_mismatches;
		return completion;
	}
	if (!enabled_ || request.job.source_revision != current_source_revision ||
		request.job.world_revision != current_world_revision ||
		!is_latest_locked(request)) {
		completion.status = WtGpuMeshingShadowCompletionStatus::Stale;
		completion.error = "GPU shadow result became stale before validation";
		++metrics_.stale_results;
		return completion;
	}
	completion.status = matched ? WtGpuMeshingShadowCompletionStatus::Matched :
		WtGpuMeshingShadowCompletionStatus::Mismatched;
	completion.error = std::move(error);
	if (matched) ++metrics_.matched_results;
	else ++metrics_.mismatched_results;
	return completion;
}

WtGpuMeshingShadowMetrics WtGpuMeshingShadowQueue::metrics() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	WtGpuMeshingShadowMetrics result = metrics_;
	result.enabled = enabled_;
	result.capacity = capacity_;
	result.queued_requests = queued_.size();
	result.in_flight_requests = in_flight_.size();
	return result;
}

bool WtGpuMeshingShadowQueue::identity_matches(
	const WtGpuMeshingShadowRequest &request,
	const WtGpuMeshingShadowIdentity &identity
) noexcept {
	return request.job.key == identity.key &&
		request.job.generation == identity.generation &&
		request.job.source_revision == identity.source_revision &&
		request.job.world_revision == identity.world_revision &&
		request.transition_mask == identity.transition_mask &&
		request.surface == identity.surface;
}

bool WtGpuMeshingShadowQueue::is_latest_locked(
	const WtGpuMeshingShadowRequest &request
) const noexcept {
	const auto newer = [&request](const WtGpuMeshingShadowRequest &candidate) {
		return candidate.job.key == request.job.key &&
			(candidate.job.generation.value > request.job.generation.value ||
			candidate.job.world_revision > request.job.world_revision);
	};
	return std::none_of(queued_.begin(), queued_.end(), newer) &&
		std::none_of(in_flight_.begin(), in_flight_.end(), newer);
}

const char *wt_gpu_meshing_shadow_surface_name(
	WtGpuMeshingShadowSurface surface
) noexcept {
	return surface == WtGpuMeshingShadowSurface::StaticWater ?
		"static_water" : "terrain";
}

const char *wt_gpu_meshing_shadow_completion_status_name(
	WtGpuMeshingShadowCompletionStatus status
) noexcept {
	switch (status) {
		case WtGpuMeshingShadowCompletionStatus::Matched: return "MATCHED";
		case WtGpuMeshingShadowCompletionStatus::Mismatched: return "MISMATCHED";
		case WtGpuMeshingShadowCompletionStatus::Stale: return "STALE";
		case WtGpuMeshingShadowCompletionStatus::UnknownRequest: return "UNKNOWN_REQUEST";
		case WtGpuMeshingShadowCompletionStatus::IdentityMismatch: return "IDENTITY_MISMATCH";
	}
	return "UNKNOWN_REQUEST";
}

} // namespace world_transvoxel
