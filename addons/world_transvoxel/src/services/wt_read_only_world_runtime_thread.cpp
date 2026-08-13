#include "services/wt_read_only_world_runtime.h"

#include "services/wt_edit_runtime_replacement.h"
#include "services/wt_page_meshing_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_storage_page_cache.h"
#include "streaming/wt_stream_scheduler.h"

#include <utility>

namespace world_transvoxel {
namespace {

constexpr std::size_t kWtPublicationPriorityBurstLimit = 16;

} // namespace

WtReadOnlyRuntimeStatus WtReadOnlyWorldRuntime::run() {
	if (!valid_) return WtReadOnlyRuntimeStatus::InvalidConfiguration;
	storage_.set_completion_notifier([this]() { notify_work(); });
	std::uint64_t observed_wake = 0;
	while (!stop_requested_.load()) {
		// Foreground edits must not sit behind a potentially large viewer-plan
		// delta. Viewer events are coalesced and may safely follow the edit; the
		// edit journal remains authoritative for chunks requested afterward.
		bool progressed = process_world_operation_event();
		progressed = process_viewer_event() || progressed;
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
		progressed = process_collision_readiness_repairs() || progressed;
		progressed = process_visual_readiness_repairs() || progressed;
		refresh_metrics_snapshot();
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
	refresh_metrics_snapshot();
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
	const bool priority = is_priority_publication(publication);
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
	const bool trace_enabled = causal_trace_.enabled();
	const WtChunkKey trace_key = trace_enabled ? publication.key : WtChunkKey{};
	const WtGenerationToken trace_generation = trace_enabled ?
		publication.generation : WtGenerationToken{};
	const std::uint64_t trace_revision = trace_enabled ?
		publication.world_revision : 0;
	const std::uint64_t trace_kind = trace_enabled ?
		static_cast<std::uint64_t>(publication.kind) : 0;
	slots[tail] = std::move(publication);
	++count;
	if (trace_enabled) {
		causal_trace_.record(
			WtCausalTraceEventKind::PublicationQueued,
			WtCausalTraceThreadRole::Runtime,
			trace_kind <= static_cast<std::uint64_t>(
				WtReadOnlyPublicationKind::CollisionPayload
			) ? &trace_key : nullptr,
			trace_generation,
			trace_revision,
			trace_kind
		);
	}
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
	const bool pop_normal =
		publication_count_ != 0 &&
		(priority_publication_count_ == 0 ||
			priority_publication_burst_ >= kWtPublicationPriorityBurstLimit);
	std::vector<WtReadOnlyPublication> *slots = pop_normal ?
		&publication_slots_ : &priority_publication_slots_;
	std::size_t *head = pop_normal ? &publication_head_ :
		&priority_publication_head_;
	std::size_t *count = pop_normal ? &publication_count_ :
		&priority_publication_count_;
	if (*count == 0) return false;
	publication = std::move((*slots)[*head]);
	(*slots)[*head] = {};
	*head = (*head + 1U) % slots->size();
	--*count;
	if (pop_normal) {
		priority_publication_burst_ = 0;
	} else {
		++priority_publication_burst_;
	}
	publication_space_available_.notify_one();
	if (causal_trace_.enabled()) {
		const std::uint64_t kind = static_cast<std::uint64_t>(publication.kind);
		causal_trace_.record(
			WtCausalTraceEventKind::PublicationPopped,
			WtCausalTraceThreadRole::Frontend,
			kind <= static_cast<std::uint64_t>(
				WtReadOnlyPublicationKind::CollisionPayload
			) ? &publication.key : nullptr,
			publication.generation,
			publication.world_revision,
			kind
		);
	}
	return true;
}

bool WtReadOnlyWorldRuntime::pop_unbudgeted_publication(
	WtReadOnlyPublication &publication
) {
	std::lock_guard<std::mutex> lock(publication_mutex_);
	const auto pop_matching = [&publication](
		std::vector<WtReadOnlyPublication> &slots,
		std::size_t &head,
		std::size_t &count
	) {
		for (std::size_t offset = 0; offset < count; ++offset) {
			const std::size_t index = (head + offset) % slots.size();
			const WtReadOnlyPublicationKind kind = slots[index].kind;
			if (kind == WtReadOnlyPublicationKind::RenderPayload ||
				kind == WtReadOnlyPublicationKind::CollisionPayload) {
				continue;
			}
			publication = std::move(slots[index]);
			for (std::size_t shift = offset; shift + 1U < count; ++shift) {
				const std::size_t destination = (head + shift) % slots.size();
				const std::size_t source = (head + shift + 1U) % slots.size();
				slots[destination] = std::move(slots[source]);
			}
			const std::size_t tail = (head + count - 1U) % slots.size();
			slots[tail] = {};
			--count;
			return true;
		}
		return false;
	};
	if (pop_matching(
			priority_publication_slots_,
			priority_publication_head_,
			priority_publication_count_
		)) {
		++priority_publication_burst_;
		publication_space_available_.notify_one();
		return true;
	}
	if (pop_matching(
			publication_slots_,
			publication_head_,
			publication_count_
		)) {
		priority_publication_burst_ = 0;
		publication_space_available_.notify_one();
		return true;
	}
	return false;
}

bool WtReadOnlyWorldRuntime::has_publication_backlog() {
	std::lock_guard<std::mutex> lock(publication_mutex_);
	return publication_count_ != 0 || priority_publication_count_ != 0;
}

bool WtReadOnlyWorldRuntime::is_priority_publication(
	const WtReadOnlyPublication &publication
) noexcept {
	switch (publication.kind) {
		case WtReadOnlyPublicationKind::ExpectChunk:
		case WtReadOnlyPublicationKind::SetCollisionRequired:
		case WtReadOnlyPublicationKind::SetVisualRequired:
		case WtReadOnlyPublicationKind::RemoveChunk:
		case WtReadOnlyPublicationKind::CollisionPayload:
			return true;
		case WtReadOnlyPublicationKind::RenderPayload:
			return publication.staged_replacement;
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

void WtReadOnlyWorldRuntime::notify_application_progress() noexcept {
	application_progress_sequence_.fetch_add(1, std::memory_order_relaxed);
	notify_work();
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
	return published_metrics_;
}

void WtReadOnlyWorldRuntime::refresh_metrics_snapshot() noexcept {
	WtReadOnlyRuntimeMetrics snapshot;
	{
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		snapshot = metrics_;
	}
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
	if (edit_replacement_) {
		const WtEditRuntimeReplacementMetrics edit =
			edit_replacement_->get_metrics();
		snapshot.edit_transaction_attempts = edit.transaction_attempts;
		snapshot.edit_completed_transactions = edit.completed_transactions;
		snapshot.edit_empty_transactions = edit.empty_transactions;
		snapshot.edit_queried_chunks = edit.queried_chunks;
		snapshot.edit_replaced_chunks = edit.replaced_chunks;
		snapshot.edit_evicted_page_entries = edit.evicted_page_entries;
		snapshot.edit_evicted_resource_entries = edit.evicted_resource_entries;
		snapshot.edit_spatial_rejections = edit.spatial_rejections;
		snapshot.edit_capacity_rejections = edit.capacity_rejections;
		snapshot.edit_state_rejections = edit.state_rejections;
		snapshot.edit_scheduler_failures = edit.scheduler_failures;
		snapshot.edit_application_failures = edit.application_failures;
		snapshot.edit_page_meshing_runtime_failures =
			edit.page_meshing_runtime_failures;
		snapshot.edit_cancelled_page_meshing_generations =
			edit.cancelled_page_meshing_generations;
	}
	const WtAsyncStorageMetrics storage = storage_.get_metrics();
	snapshot.storage_queued_requests = storage_.queued_request_count();
	snapshot.storage_queued_completions = storage_.queued_completion_count();
	snapshot.storage_active_requests = storage_.active_request_count();
	snapshot.storage_accepted_requests = storage.accepted_requests;
	snapshot.storage_started_requests = storage.started_requests;
	snapshot.storage_completed_requests = storage.completed_requests;
	snapshot.storage_request_queue_rejections =
		storage.request_queue_rejections;
	snapshot.storage_duplicate_requests = storage.duplicate_requests;
	snapshot.storage_successful_pages = storage.successful_pages;
	snapshot.storage_load_time_ns_last = storage.load_time_ns_last;
	snapshot.storage_load_time_ns_total = storage.load_time_ns_total;
	snapshot.storage_load_time_ns_maximum = storage.load_time_ns_maximum;
	snapshot.storage_worker_count = storage.worker_count;
	snapshot.storage_in_flight_requests = storage.in_flight_requests;
	snapshot.storage_maximum_in_flight_requests =
		storage.maximum_in_flight_requests;
	snapshot.storage_in_flight_elapsed_ns = storage.in_flight_elapsed_ns;
	snapshot.storage_in_flight_key_x = storage.in_flight_key_x;
	snapshot.storage_in_flight_key_y = storage.in_flight_key_y;
	snapshot.storage_in_flight_key_z = storage.in_flight_key_z;
	snapshot.storage_in_flight_key_lod = storage.in_flight_key_lod;
	snapshot.storage_in_flight_generation = storage.in_flight_generation;
	if (lod_planner_) {
		const WtPageHierarchyMetrics hierarchy =
			lod_planner_->hierarchy_metrics();
		snapshot.hierarchy_kind = static_cast<std::uint64_t>(hierarchy.kind);
		snapshot.hierarchy_declared_pages = hierarchy.declared_page_count;
		snapshot.hierarchy_explicit_index_entries =
			hierarchy.explicit_index_entries;
		snapshot.hierarchy_estimated_index_bytes =
			hierarchy.estimated_index_bytes;
		snapshot.hierarchy_membership_queries = hierarchy.membership_queries;
		snapshot.hierarchy_child_queries = hierarchy.child_queries;
		snapshot.hierarchy_ancestor_queries = hierarchy.ancestor_queries;
		snapshot.hierarchy_neighbor_queries = hierarchy.neighbor_queries;
		snapshot.hierarchy_range_queries = hierarchy.range_queries;
		snapshot.hierarchy_viewer_root_queries = hierarchy.viewer_root_queries;
		snapshot.hierarchy_lod_enumerations = hierarchy.lod_enumerations;
	}
	snapshot.hierarchy_sparse_overlay_entries = storage_.overlay_page_count();
	snapshot.hierarchy_sparse_overlay_index_bytes =
		storage_.overlay_index_bytes();
	if (page_runtime_) {
		const WtPageMeshingRuntimeMetrics page = page_runtime_->get_metrics();
		snapshot.page_sample_failures = page.sample_failures;
		snapshot.page_mesh_failures = page.mesh_failures;
		snapshot.page_storage_failures = page.storage_failures;
		snapshot.page_cache_failures = page.cache_failures;
		snapshot.page_scheduler_backpressure = page.scheduler_backpressure;
		snapshot.page_dependency_requests = page.dependency_requests;
		snapshot.page_dependency_reprioritizations =
			page.dependency_reprioritizations;
		snapshot.page_dependency_cache_hits = page.dependency_cache_hits;
		snapshot.page_dependency_cache_misses = page.dependency_cache_misses;
		snapshot.page_accepted_storage_completions =
			page.accepted_storage_completions;
		snapshot.page_stale_storage_completions =
			page.stale_storage_completions;
		snapshot.page_loading_records = page.loading_records;
		snapshot.page_sample_ready_records = page.sample_ready_records;
		snapshot.page_awaiting_mesh_records = page.awaiting_mesh_records;
		snapshot.page_mesh_ready_records = page.mesh_ready_records;
		snapshot.page_ready_records = page.ready_records;
		snapshot.page_unresolved_dependencies =
			page.unresolved_dependencies;
		snapshot.page_pending_dependency_requests =
			page.pending_dependency_requests;
		snapshot.page_pinned_pages = page.pinned_pages;
		snapshot.page_last_failure_key_x = page.last_failure_key_x;
		snapshot.page_last_failure_key_y = page.last_failure_key_y;
		snapshot.page_last_failure_key_z = page.last_failure_key_z;
		snapshot.page_last_failure_key_lod = page.last_failure_key_lod;
	}
	if (page_cache_) {
		const WtStoragePageCacheMetrics cache = page_cache_->get_metrics();
		snapshot.page_cache_encoded_entries = page_cache_->encoded_entry_count();
		snapshot.page_cache_decoded_entries = page_cache_->decoded_entry_count();
		snapshot.page_cache_encoded_hits = cache.encoded_hits;
		snapshot.page_cache_encoded_misses = cache.encoded_misses;
		snapshot.page_cache_encoded_insertions = cache.encoded_insertions;
		snapshot.page_cache_encoded_refreshes = cache.encoded_refreshes;
		snapshot.page_cache_encoded_evictions = cache.encoded_evictions;
		snapshot.page_cache_decoded_hits = cache.decoded_hits;
		snapshot.page_cache_decoded_misses = cache.decoded_misses;
		snapshot.page_cache_decoded_insertions = cache.decoded_insertions;
		snapshot.page_cache_decoded_evictions = cache.decoded_evictions;
	}
	{
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		published_metrics_ = snapshot;
	}
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
		case WtReadOnlyRuntimeStatus::PipelineStorageCompletionFailure:
			return "read-only storage completion pipeline failed";
		case WtReadOnlyRuntimeStatus::PipelineSchedulerJobFailure:
			return "read-only scheduler job pipeline failed";
		case WtReadOnlyRuntimeStatus::PipelineTerrainMeshCompletionFailure:
			return "read-only terrain mesh completion pipeline failed";
		case WtReadOnlyRuntimeStatus::PipelineRenderCompletionFailure:
			return "read-only render completion pipeline failed";
		case WtReadOnlyRuntimeStatus::PipelineTransitionMaskUpdateFailure:
			return "read-only transition-mask update pipeline failed";
		case WtReadOnlyRuntimeStatus::PipelineCollisionRebuildFailure:
			return "read-only collision rebuild pipeline failed";
		case WtReadOnlyRuntimeStatus::PipelineCollisionRepairFailure:
			return "read-only collision repair pipeline failed";
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
