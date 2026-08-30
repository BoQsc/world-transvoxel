#include "diagnostics/wt_gpu_meshing_shadow.h"

#include <algorithm>
#include <utility>

namespace world_transvoxel {

bool WtGpuMeshingShadowQueue::begin(
	std::size_t capacity,
	bool retain_publication_authority,
	WtGpuMeshingCaptureStage capture_stage
) {
	if (capacity == 0 || capacity > 16) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	enabled_ = true;
	retain_publication_authority_ = retain_publication_authority;
	capture_stage_ = capture_stage;
	capacity_ = capacity;
	next_request_id_ = 1;
	next_reservation_id_ = 1;
	queued_.clear();
	in_flight_.clear();
	capture_reservations_.clear();
	metrics_ = {};
	metrics_.enabled = true;
	metrics_.capacity = capacity;
	return true;
}

void WtGpuMeshingShadowQueue::end() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		enabled_ = false;
		retain_publication_authority_ = false;
		capture_stage_ = WtGpuMeshingCaptureStage::PostMeshAuthority;
		capacity_ = 0;
		queued_.clear();
		in_flight_.clear();
		capture_reservations_.clear();
		metrics_.enabled = false;
		metrics_.capacity = 0;
		metrics_.queued_requests = 0;
		metrics_.in_flight_requests = 0;
		metrics_.reserved_capture_slots = 0;
	}
	notify_capacity_available();
}

bool WtGpuMeshingShadowQueue::enabled() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	return enabled_;
}

bool WtGpuMeshingShadowQueue::captures_pre_mesh_field() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	return enabled_ && capture_stage_ == WtGpuMeshingCaptureStage::PreMeshField;
}

void WtGpuMeshingShadowQueue::set_capacity_available_notifier(
	std::function<void()> notifier
) {
	std::lock_guard<std::mutex> lock(notifier_mutex_);
	capacity_available_notifier_ = std::move(notifier);
}

std::uint64_t WtGpuMeshingShadowQueue::reserve_capture_slots(
	const WtChunkJob &job
) {
	std::lock_guard<std::mutex> lock(mutex_);
	++metrics_.capture_reservation_attempts;
	if (!enabled_) {
		++metrics_.capture_reservation_rejections;
		return 0;
	}
	const std::size_t requested_slots = capacity_ >= 2 ? 2U : 1U;
	std::size_t occupied = queued_.size() + in_flight_.size() +
		metrics_.reserved_capture_slots;
	if (occupied <= capacity_ && requested_slots > capacity_ - occupied) {
		const std::size_t required = requested_slots - (capacity_ - occupied);
		std::size_t released = 0;
		while (released < required) {
			const auto candidate = std::min_element(
				queued_.begin(), queued_.end(),
				[](const WtGpuMeshingShadowRequest &left,
					const WtGpuMeshingShadowRequest &right) {
					if (left.job.priority != right.job.priority) {
						return left.job.priority < right.job.priority;
					}
					return left.job.sequence < right.job.sequence;
				}
			);
			const bool lossless_gpu_only_admission =
				retain_publication_authority_ &&
				capture_stage_ == WtGpuMeshingCaptureStage::PreMeshField;
			if (candidate == queued_.end() ||
				!(lossless_gpu_only_admission ?
					job_version_supersedes(job, candidate->job) :
					job_supersedes(job, candidate->job))) {
				break;
			}
			const WtChunkJob superseded_job = candidate->job;
			const std::size_t before = queued_.size();
			queued_.erase(
				std::remove_if(
					queued_.begin(), queued_.end(),
					[&superseded_job](
						const WtGpuMeshingShadowRequest &request
					) {
						return same_job_version(
							request.job, superseded_job
						);
					}
				),
				queued_.end()
			);
			const std::size_t removed = before - queued_.size();
			if (removed == 0) break;
			released += removed;
			metrics_.superseded_queued_requests += removed;
		}
		metrics_.queued_requests = queued_.size();
		occupied = queued_.size() + in_flight_.size() +
			metrics_.reserved_capture_slots;
	}
	if (occupied > capacity_ || requested_slots > capacity_ - occupied) {
		++metrics_.capture_reservation_rejections;
		return 0;
	}
	const std::uint64_t reservation_id = next_reservation_id_++;
	capture_reservations_.push_back({ reservation_id, requested_slots, job });
	metrics_.reserved_capture_slots += requested_slots;
	return reservation_id;
}

bool WtGpuMeshingShadowQueue::capture_reserved(
	std::uint64_t reservation_id,
	WtGpuMeshingShadowCapture capture
) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (reservation_id == 0 ||
		(capture.records.empty() && capture.retained_pages.empty())) {
		++metrics_.reserved_capture_failures;
		return false;
	}
	if (!enabled_ || capture.capture_stage != capture_stage_) {
		++metrics_.reserved_capture_failures;
		return false;
	}
	const auto reservation = std::find_if(
		capture_reservations_.begin(), capture_reservations_.end(),
		[reservation_id](const CaptureReservation &candidate) {
			return candidate.id == reservation_id;
		}
	);
	if (reservation == capture_reservations_.end() ||
		reservation->remaining_slots == 0 ||
		!same_job_version(reservation->job, capture.job)) {
		++metrics_.reserved_capture_failures;
		return false;
	}
	const bool cpu_visual_mesh_omitted = capture.cpu_visual_mesh_omitted;
	WtGpuMeshingShadowRequest request;
	static_cast<WtGpuMeshingShadowCapture &>(request) = std::move(capture);
	if (!retain_publication_authority_) {
		request.retained_pages.clear();
		request.authority_terrain_mesh.reset();
		request.authority_water_mesh.reset();
	}
	--reservation->remaining_slots;
	--metrics_.reserved_capture_slots;
	request.request_id = next_request_id_++;
	const auto replace = std::find_if(
		queued_.begin(), queued_.end(),
		[&request](const WtGpuMeshingShadowRequest &queued) {
			return supersedes_queued(request, queued);
		}
	);
	if (replace != queued_.end()) {
		*replace = std::move(request);
		++metrics_.superseded_queued_requests;
	} else {
		queued_.push_back(std::move(request));
	}
	if (reservation->remaining_slots == 0) {
		capture_reservations_.erase(reservation);
	}
	++metrics_.captured_requests;
	++metrics_.reserved_captures;
	if (capture_stage_ == WtGpuMeshingCaptureStage::PreMeshField) {
		++metrics_.pre_mesh_field_captures;
	}
	if (cpu_visual_mesh_omitted) {
		++metrics_.cpu_visual_mesh_omitted_captures;
	}
	metrics_.queued_requests = queued_.size();
	return true;
}

void WtGpuMeshingShadowQueue::release_capture_slots(
	std::uint64_t reservation_id
) noexcept {
	if (reservation_id == 0) return;
	std::lock_guard<std::mutex> lock(mutex_);
	const auto reservation = std::find_if(
		capture_reservations_.begin(), capture_reservations_.end(),
		[reservation_id](const CaptureReservation &candidate) {
			return candidate.id == reservation_id;
		}
	);
	if (reservation == capture_reservations_.end()) return;
	metrics_.reserved_capture_slots -= reservation->remaining_slots;
	metrics_.released_capture_slots += reservation->remaining_slots;
	capture_reservations_.erase(reservation);
	notify_capacity_available();
}

bool WtGpuMeshingShadowQueue::capture(WtGpuMeshingShadowCapture capture) {
	if (capture.records.empty() && capture.retained_pages.empty()) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	if (!enabled_ || capture.capture_stage != capture_stage_) return false;
	const bool cpu_visual_mesh_omitted = capture.cpu_visual_mesh_omitted;
	WtGpuMeshingShadowRequest request;
	static_cast<WtGpuMeshingShadowCapture &>(request) = std::move(capture);
	if (!retain_publication_authority_) {
		request.retained_pages.clear();
		request.authority_terrain_mesh.reset();
		request.authority_water_mesh.reset();
	}
	if (queued_.size() + in_flight_.size() +
			metrics_.reserved_capture_slots >= capacity_) {
		const auto replace = std::find_if(
			queued_.begin(), queued_.end(),
			[&request](const WtGpuMeshingShadowRequest &queued) {
				return supersedes_queued(request, queued);
			}
		);
		if (replace == queued_.end()) {
			++metrics_.capacity_rejections;
			return false;
		}
		request.request_id = next_request_id_++;
		*replace = std::move(request);
		++metrics_.captured_requests;
		if (capture_stage_ == WtGpuMeshingCaptureStage::PreMeshField) {
			++metrics_.pre_mesh_field_captures;
		}
		if (cpu_visual_mesh_omitted) {
			++metrics_.cpu_visual_mesh_omitted_captures;
		}
		++metrics_.superseded_queued_requests;
		metrics_.queued_requests = queued_.size();
		return true;
	}
	request.request_id = next_request_id_++;
	queued_.push_back(std::move(request));
	++metrics_.captured_requests;
	if (capture_stage_ == WtGpuMeshingCaptureStage::PreMeshField) {
		++metrics_.pre_mesh_field_captures;
	}
	if (cpu_visual_mesh_omitted) {
		++metrics_.cpu_visual_mesh_omitted_captures;
	}
	metrics_.queued_requests = queued_.size();
	return true;
}

bool WtGpuMeshingShadowQueue::pop(WtGpuMeshingShadowRequest &request) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!enabled_ || queued_.empty()) return false;
	const auto selected = std::min_element(
		queued_.begin(), queued_.end(),
		[](const WtGpuMeshingShadowRequest &left,
			const WtGpuMeshingShadowRequest &right) {
			return job_precedes(left.job, right.job);
		}
	);
	const WtChunkJob selected_job = selected->job;
	const bool priority_dequeue = selected != queued_.begin();
	request = std::move(*selected);
	queued_.erase(selected);
	const std::size_t before_coalesce = queued_.size();
	queued_.erase(
		std::remove_if(
			queued_.begin(), queued_.end(),
			[&selected_job](const WtGpuMeshingShadowRequest &candidate) {
				if (candidate.job.source_revision <
					selected_job.source_revision) {
					return true;
				}
				return candidate.job.key == selected_job.key &&
					candidate.job.source_revision ==
						selected_job.source_revision &&
					(candidate.job.world_revision < selected_job.world_revision ||
					(candidate.job.world_revision == selected_job.world_revision &&
						candidate.job.generation.value <
							selected_job.generation.value));
			}
		),
		queued_.end()
	);
	const std::size_t coalesced = before_coalesce - queued_.size();
	if (priority_dequeue) ++metrics_.priority_dequeues;
	metrics_.dequeue_superseded_requests += coalesced;
	metrics_.superseded_queued_requests += coalesced;
	if (coalesced != 0) notify_capacity_available();
	WtGpuMeshingShadowRequest tracked_request;
	tracked_request.job = request.job;
	tracked_request.transition_mask = request.transition_mask;
	tracked_request.cached_transition_mask = request.cached_transition_mask;
	tracked_request.surface = request.surface;
	tracked_request.capture_stage = request.capture_stage;
	tracked_request.static_water_surface_expected =
		request.static_water_surface_expected;
	tracked_request.cpu_visual_mesh_omitted = request.cpu_visual_mesh_omitted;
	tracked_request.request_id = request.request_id;
	tracked_request.retained_pages = request.retained_pages;
	tracked_request.authority_terrain_mesh = request.authority_terrain_mesh;
	tracked_request.authority_water_mesh = request.authority_water_mesh;
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
	notify_capacity_available();
	completion.retained_request = std::move(request);
	completion.has_retained_request = true;
	const WtGpuMeshingShadowRequest &retained = completion.retained_request;
	if (!identity_matches(retained, identity)) {
		completion.status = WtGpuMeshingShadowCompletionStatus::IdentityMismatch;
		completion.error = "GPU shadow result identity does not match its request";
		++metrics_.identity_mismatches;
		return completion;
	}
	if (!enabled_ || retained.job.source_revision != current_source_revision ||
		retained.job.world_revision != current_world_revision ||
		!is_latest_locked(retained)) {
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

WtGpuMeshingResidentValidation WtGpuMeshingShadowQueue::validate_resident(
	std::uint64_t request_id,
	const WtGpuMeshingShadowIdentity &identity,
	std::uint64_t current_source_revision
) {
	std::lock_guard<std::mutex> lock(mutex_);
	WtGpuMeshingResidentValidation validation;
	validation.request_id = request_id;
	const auto found = std::find_if(
		in_flight_.begin(), in_flight_.end(),
		[request_id](const WtGpuMeshingShadowRequest &request) {
			return request.request_id == request_id;
		}
	);
	if (found == in_flight_.end()) {
		++metrics_.unknown_results;
		validation.error = "GPU resident request is not in flight";
		return validation;
	}
	WtGpuMeshingShadowRequest request = std::move(*found);
	in_flight_.erase(found);
	metrics_.in_flight_requests = in_flight_.size();
	notify_capacity_available();
	if (!identity_matches(request, identity)) {
		validation.status =
			WtGpuMeshingResidentValidationStatus::IdentityMismatch;
		validation.error =
			"GPU resident result identity does not match its request";
		++metrics_.identity_mismatches;
		return validation;
	}
	if (!enabled_ || request.job.source_revision != current_source_revision ||
		!is_latest_locked(request)) {
		validation.status = WtGpuMeshingResidentValidationStatus::Stale;
		validation.error =
			"GPU resident result became stale before admission";
		++metrics_.resident_stale_results;
		return validation;
	}
	validation.status = WtGpuMeshingResidentValidationStatus::Ready;
	++metrics_.resident_ready_results;
	return validation;
}

WtGpuMeshingResidentValidation WtGpuMeshingShadowQueue::reject_resident(
	std::uint64_t request_id,
	const WtGpuMeshingShadowIdentity &identity,
	std::string error
) {
	std::lock_guard<std::mutex> lock(mutex_);
	WtGpuMeshingResidentValidation validation;
	validation.request_id = request_id;
	const auto found = std::find_if(
		in_flight_.begin(), in_flight_.end(),
		[request_id](const WtGpuMeshingShadowRequest &request) {
			return request.request_id == request_id;
		}
	);
	if (found == in_flight_.end()) {
		++metrics_.unknown_results;
		validation.error = "GPU resident request is not in flight";
		return validation;
	}
	WtGpuMeshingShadowRequest request = std::move(*found);
	in_flight_.erase(found);
	metrics_.in_flight_requests = in_flight_.size();
	notify_capacity_available();
	if (!identity_matches(request, identity)) {
		validation.status =
			WtGpuMeshingResidentValidationStatus::IdentityMismatch;
		validation.error =
			"Rejected GPU resident identity does not match its request";
		++metrics_.identity_mismatches;
		return validation;
	}
	validation.status = WtGpuMeshingResidentValidationStatus::Rejected;
	validation.error = std::move(error);
	++metrics_.resident_rejected_results;
	return validation;
}

WtGpuMeshingShadowMetrics WtGpuMeshingShadowQueue::metrics() const noexcept {
	std::lock_guard<std::mutex> lock(mutex_);
	WtGpuMeshingShadowMetrics result = metrics_;
	result.enabled = enabled_;
	result.capacity = capacity_;
	result.queued_requests = queued_.size();
	result.in_flight_requests = in_flight_.size();
	if (!in_flight_.empty()) {
		const WtGpuMeshingShadowRequest &oldest = in_flight_.front();
		result.has_oldest_in_flight_request = true;
		result.oldest_in_flight_request_id = oldest.request_id;
		result.oldest_in_flight_identity.key = oldest.job.key;
		result.oldest_in_flight_identity.generation = oldest.job.generation;
		result.oldest_in_flight_identity.source_revision =
			oldest.job.source_revision;
		result.oldest_in_flight_identity.world_revision =
			oldest.job.world_revision;
		result.oldest_in_flight_identity.transition_mask =
			oldest.transition_mask;
		result.oldest_in_flight_identity.surface = oldest.surface;
	}
	return result;
}

void WtGpuMeshingShadowQueue::notify_capacity_available() const {
	std::function<void()> notifier;
	{
		std::lock_guard<std::mutex> lock(notifier_mutex_);
		notifier = capacity_available_notifier_;
	}
	if (notifier) notifier();
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

bool WtGpuMeshingShadowQueue::supersedes_queued(
	const WtGpuMeshingShadowCapture &capture,
	const WtGpuMeshingShadowRequest &queued
) noexcept {
	if (capture.job.source_revision > queued.job.source_revision) return true;
	if (capture.job.source_revision < queued.job.source_revision) return false;
	if (capture.job.key != queued.job.key) return false;
	if (capture.job.world_revision > queued.job.world_revision) return true;
	if (capture.job.world_revision < queued.job.world_revision) return false;
	return capture.job.generation.value > queued.job.generation.value;
}

bool WtGpuMeshingShadowQueue::same_job_version(
	const WtChunkJob &left,
	const WtChunkJob &right
) noexcept {
	return left.key == right.key &&
		left.generation == right.generation &&
		left.source_revision == right.source_revision &&
		left.world_revision == right.world_revision;
}

bool WtGpuMeshingShadowQueue::job_supersedes(
	const WtChunkJob &incoming,
	const WtChunkJob &queued
) noexcept {
	if (incoming.source_revision != queued.source_revision) {
		return incoming.source_revision > queued.source_revision;
	}
	if (incoming.key == queued.key &&
		incoming.world_revision != queued.world_revision) {
		return incoming.world_revision > queued.world_revision;
	}
	if (incoming.key == queued.key &&
		incoming.generation != queued.generation) {
		return incoming.generation.value > queued.generation.value;
	}
	return incoming.priority > queued.priority;
}

bool WtGpuMeshingShadowQueue::job_version_supersedes(
	const WtChunkJob &incoming,
	const WtChunkJob &queued
) noexcept {
	if (incoming.source_revision != queued.source_revision) {
		return incoming.source_revision > queued.source_revision;
	}
	if (incoming.key != queued.key) return false;
	if (incoming.world_revision != queued.world_revision) {
		return incoming.world_revision > queued.world_revision;
	}
	return incoming.generation.value > queued.generation.value;
}

bool WtGpuMeshingShadowQueue::job_precedes(
	const WtChunkJob &left,
	const WtChunkJob &right
) noexcept {
	if (left.source_revision != right.source_revision) {
		return left.source_revision > right.source_revision;
	}
	if (left.world_revision != right.world_revision) {
		return left.world_revision > right.world_revision;
	}
	if (left.priority != right.priority) {
		return left.priority > right.priority;
	}
	if (left.key == right.key && left.generation != right.generation) {
		return left.generation.value > right.generation.value;
	}
	return left.sequence < right.sequence;
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

const char *wt_gpu_meshing_capture_stage_name(
	WtGpuMeshingCaptureStage stage
) noexcept {
	return stage == WtGpuMeshingCaptureStage::PreMeshField ?
		"pre_mesh_field" : "post_mesh_authority";
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

const char *wt_gpu_meshing_resident_validation_status_name(
	WtGpuMeshingResidentValidationStatus status
) noexcept {
	switch (status) {
		case WtGpuMeshingResidentValidationStatus::Ready: return "READY";
		case WtGpuMeshingResidentValidationStatus::Rejected: return "REJECTED";
		case WtGpuMeshingResidentValidationStatus::Stale: return "STALE";
		case WtGpuMeshingResidentValidationStatus::UnknownRequest:
			return "UNKNOWN_REQUEST";
		case WtGpuMeshingResidentValidationStatus::IdentityMismatch:
			return "IDENTITY_MISMATCH";
	}
	return "UNKNOWN_REQUEST";
}

} // namespace world_transvoxel
