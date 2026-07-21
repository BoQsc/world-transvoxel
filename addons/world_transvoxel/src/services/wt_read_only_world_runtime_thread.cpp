#include "services/wt_read_only_world_runtime.h"

#include "services/wt_page_meshing_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "streaming/wt_stream_scheduler.h"

#include <utility>

namespace world_transvoxel {

WtReadOnlyRuntimeStatus WtReadOnlyWorldRuntime::run() {
	if (!valid_) return WtReadOnlyRuntimeStatus::InvalidConfiguration;
	storage_.set_completion_notifier([this]() { notify_work(); });
	std::uint64_t observed_wake = 0;
	while (!stop_requested_.load()) {
		bool progressed = process_viewer_event();
		progressed = process_world_operation_event() || progressed;
		progressed = process_storage_completions() || progressed;
		progressed = page_runtime_->resume_loading_records(
			storage_,
			*page_cache_,
			*scheduler_,
			4
		) != 0 || progressed;
		progressed = page_runtime_->flush_scheduler_results(*scheduler_) != 0 ||
			progressed;
		progressed = scheduler_->apply_completions(
			static_cast<std::size_t>(config_.active_chunk_capacity)
		) != 0 || progressed;
		progressed = process_pending_transition_remeshes() || progressed;
		progressed = process_scheduler_jobs() || progressed;
		progressed = scheduler_->apply_completions(
			static_cast<std::size_t>(config_.active_chunk_capacity)
		) != 0 || progressed;
		progressed = process_pending_transition_remeshes() || progressed;
		progressed = process_mesh_completions() || progressed;
		progressed = process_visual_readiness_repairs() || progressed;
		if (last_status_.load() != WtReadOnlyRuntimeStatus::Ok) break;
		if (!progressed) {
			std::unique_lock<std::mutex> lock(wake_mutex_);
			observed_wake = wake_sequence_;
			wake_condition_.wait(lock, [&]() {
				return stop_requested_.load() ||
					wake_sequence_ != observed_wake;
			});
		}
	}
	storage_.set_completion_notifier({});
	return last_status_.load();
}

void WtReadOnlyWorldRuntime::request_stop() noexcept {
	stop_requested_.store(true);
	notify_work();
	publication_space_available_.notify_all();
}

bool WtReadOnlyWorldRuntime::push_publication(
	WtReadOnlyPublication publication
) {
	std::unique_lock<std::mutex> lock(publication_mutex_);
	const bool priority = is_priority_publication(publication.kind);
	std::vector<WtReadOnlyPublication> &slots = priority ?
		priority_publication_slots_ : publication_slots_;
	std::size_t &head = priority ?
		priority_publication_head_ : publication_head_;
	std::size_t &count = priority ?
		priority_publication_count_ : publication_count_;
	publication_space_available_.wait(lock, [&]() {
		return stop_requested_.load() ||
			count < slots.size();
	});
	if (stop_requested_.load()) return false;
	const std::size_t tail = (head + count) % slots.size();
	slots[tail] = std::move(publication);
	++count;
	{
		std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
		++metrics_.published_events;
	}
	return true;
}

bool WtReadOnlyWorldRuntime::pop_publication(
	WtReadOnlyPublication &publication
) {
	std::lock_guard<std::mutex> lock(publication_mutex_);
	std::vector<WtReadOnlyPublication> *slots =
		&priority_publication_slots_;
	std::size_t *head = &priority_publication_head_;
	std::size_t *count = &priority_publication_count_;
	if (*count == 0) {
		slots = &publication_slots_;
		head = &publication_head_;
		count = &publication_count_;
	}
	if (*count == 0) return false;
	publication = std::move((*slots)[*head]);
	(*slots)[*head] = {};
	*head = (*head + 1U) % slots->size();
	--*count;
	publication_space_available_.notify_one();
	return true;
}

bool WtReadOnlyWorldRuntime::is_priority_publication(
	WtReadOnlyPublicationKind kind
) noexcept {
	switch (kind) {
		case WtReadOnlyPublicationKind::ExpectChunk:
		case WtReadOnlyPublicationKind::SetCollisionRequired:
		case WtReadOnlyPublicationKind::SetVisualRequired:
		case WtReadOnlyPublicationKind::RemoveChunk:
		case WtReadOnlyPublicationKind::CollisionPayload:
			return true;
		case WtReadOnlyPublicationKind::RenderPayload:
		case WtReadOnlyPublicationKind::EditCommitted:
		case WtReadOnlyPublicationKind::EditRejected:
		case WtReadOnlyPublicationKind::AuthoritativeSampleReady:
		case WtReadOnlyPublicationKind::AuthoritativeSampleRejected:
		case WtReadOnlyPublicationKind::AuthoritativeSampleBatchReady:
		case WtReadOnlyPublicationKind::AuthoritativeSampleBatchRejected:
		case WtReadOnlyPublicationKind::WorldSnapshotReady:
		case WtReadOnlyPublicationKind::WorldSnapshotRejected:
			return false;
	}
	return false;
}

void WtReadOnlyWorldRuntime::notify_work() noexcept {
	{
		std::lock_guard<std::mutex> lock(wake_mutex_);
		++wake_sequence_;
	}
	wake_condition_.notify_one();
}

void WtReadOnlyWorldRuntime::set_failure(
	WtReadOnlyRuntimeStatus status
) noexcept {
	WtReadOnlyRuntimeStatus expected = WtReadOnlyRuntimeStatus::Ok;
	last_status_.compare_exchange_strong(expected, status);
	notify_work();
}

WtReadOnlyRuntimeStatus WtReadOnlyWorldRuntime::last_status() const noexcept {
	return last_status_.load();
}

WtReadOnlyRuntimeMetrics
WtReadOnlyWorldRuntime::get_metrics() const noexcept {
	std::lock_guard<std::mutex> lock(metrics_mutex_);
	WtReadOnlyRuntimeMetrics snapshot = metrics_;
	if (scheduler_) {
		const WtSchedulerMetrics scheduler = scheduler_->get_metrics();
		snapshot.scheduler_requested_records = scheduler.requested_records;
		snapshot.scheduler_sampling_records = scheduler.sampling_records;
		snapshot.scheduler_meshing_records = scheduler.meshing_records;
		snapshot.scheduler_ready_records = scheduler.ready_records;
		snapshot.scheduler_failed_records = scheduler.failed_records;
		snapshot.scheduler_queued_jobs = scheduler_->queued_job_count();
		snapshot.scheduler_queued_completions =
			scheduler_->queued_completion_count();
		snapshot.scheduler_queue_rejections = scheduler.queue_rejections;
	}
	if (page_runtime_) {
		const WtPageMeshingRuntimeMetrics page = page_runtime_->get_metrics();
		snapshot.page_sample_failures = page.sample_failures;
		snapshot.page_mesh_failures = page.mesh_failures;
		snapshot.page_storage_failures = page.storage_failures;
		snapshot.page_cache_failures = page.cache_failures;
		snapshot.page_scheduler_backpressure = page.scheduler_backpressure;
		snapshot.page_last_failure_key_x = page.last_failure_key_x;
		snapshot.page_last_failure_key_y = page.last_failure_key_y;
		snapshot.page_last_failure_key_z = page.last_failure_key_z;
		snapshot.page_last_failure_key_lod = page.last_failure_key_lod;
	}
	return snapshot;
}

std::uint64_t WtReadOnlyWorldRuntime::world_revision() const noexcept {
	return world_revision_.load();
}

const char *wt_read_only_runtime_status_message(
	WtReadOnlyRuntimeStatus status
) noexcept {
	switch (status) {
		case WtReadOnlyRuntimeStatus::Ok: return "ok";
		case WtReadOnlyRuntimeStatus::InvalidConfiguration:
			return "read-only runtime configuration is invalid";
		case WtReadOnlyRuntimeStatus::NotRunning:
			return "world is not running";
		case WtReadOnlyRuntimeStatus::InvalidViewer:
			return "viewer event is invalid";
		case WtReadOnlyRuntimeStatus::ViewerQueueFull:
			return "viewer event queue is full";
		case WtReadOnlyRuntimeStatus::InvalidEdit:
			return "edit transaction is invalid";
		case WtReadOnlyRuntimeStatus::EditQueueFull:
			return "edit transaction queue is full";
		case WtReadOnlyRuntimeStatus::EditFailure:
			return "edit transaction runtime integration failed";
		case WtReadOnlyRuntimeStatus::InvalidQuery:
			return "authoritative sample query is invalid";
		case WtReadOnlyRuntimeStatus::InvalidSnapshot:
			return "world snapshot request is invalid";
		case WtReadOnlyRuntimeStatus::OperationQueueFull:
			return "world operation queue is full";
		case WtReadOnlyRuntimeStatus::DesiredSetFailure:
			return "viewer desired-set update failed";
		case WtReadOnlyRuntimeStatus::RuntimeDeltaFailure:
			return "streaming runtime delta failed";
		case WtReadOnlyRuntimeStatus::RuntimeDeltaChangeCapacityExceeded:
			return "streaming runtime delta exceeded change capacity";
		case WtReadOnlyRuntimeStatus::RuntimeDeltaStateMismatch:
			return "streaming runtime delta state mismatch";
		case WtReadOnlyRuntimeStatus::RuntimeDeltaRecordCapacityExceeded:
			return "streaming runtime delta exceeded record capacity";
		case WtReadOnlyRuntimeStatus::RuntimeDeltaJobQueueCapacityExceeded:
			return "streaming runtime delta exceeded job queue capacity";
		case WtReadOnlyRuntimeStatus::RuntimeDeltaSchedulerFailure:
			return "streaming runtime delta scheduler operation failed";
		case WtReadOnlyRuntimeStatus::RuntimeDeltaApplicationFailure:
			return "streaming runtime delta application operation failed";
		case WtReadOnlyRuntimeStatus::RuntimeDeltaPageMeshingRuntimeFailure:
			return "streaming runtime delta page meshing runtime operation failed";
		case WtReadOnlyRuntimeStatus::PipelineFailure:
			return "read-only page or meshing pipeline failed";
		case WtReadOnlyRuntimeStatus::PublicationFailure:
			return "read-only publication queue failed";
	}
	return "unknown read-only runtime status";
}

const char *wt_read_only_edit_status_message(
	WtReadOnlyEditStatus status
) noexcept {
	switch (status) {
		case WtReadOnlyEditStatus::Ok: return "ok";
		case WtReadOnlyEditStatus::InvalidTransaction:
			return "edit transaction is invalid";
		case WtReadOnlyEditStatus::StaleRevision:
			return "edit transaction world revision is stale";
		case WtReadOnlyEditStatus::SpatialFailure:
			return "edit transaction affected-chunk query failed";
		case WtReadOnlyEditStatus::JournalFailure:
			return "edit transaction durable journal append failed";
		case WtReadOnlyEditStatus::ReplacementFailure:
			return "edit transaction chunk replacement failed";
	}
	return "unknown edit transaction status";
}

} // namespace world_transvoxel
