#include "services/wt_read_only_world_runtime.h"

#include "services/wt_chunk_resource_cache.h"
#include "services/wt_chunk_application.h"
#include "services/wt_edit_runtime_replacement.h"
#include "services/wt_page_meshing_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_edit_journal_store.h"
#include "storage/wt_storage_page_cache.h"
#include "streaming/wt_stream_scheduler.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace world_transvoxel {
namespace {

constexpr std::size_t kWtPublicationPriorityBurstLimit = 16;

} // namespace

WtReadOnlyRuntimeStatus WtReadOnlyWorldRuntime::run() {
	if (!valid_) return WtReadOnlyRuntimeStatus::InvalidConfiguration;
	storage_.set_completion_notifier([this]() { notify_work(); });
	page_runtime_->set_mesh_completion_notifier([this]() { notify_work(); });
	if (gpu_meshing_shadow_) {
		gpu_meshing_shadow_->set_capacity_available_notifier(
			[this]() { notify_work(); }
		);
	}
	std::uint64_t observed_wake = 0;
	while (!stop_requested_.load()) {
		if (edit_journal_store_ != nullptr &&
			edit_journal_store_->deferred_status() !=
				WtEditJournalStoreStatus::Ok) {
			set_failure(WtReadOnlyRuntimeStatus::EditFailure);
			break;
		}
		// Foreground edits must not sit behind a potentially large viewer-plan
		// delta. Viewer events are coalesced and may safely follow the edit; the
		// edit journal remains authoritative for chunks requested afterward.
		bool progressed = process_world_operation_event();
		// A reserved interaction worker can finish between runtime iterations.
		// Consume that result before entering scheduler dispatch, where preparing a
		// cold background mesh may copy or capture many page dependencies.
		progressed = process_async_mesh_completions() || progressed;
		// An accepted edit owns the highest scheduler priority. Admit its sample
		// before planning another viewer delta so a cached page can reach the
		// mesh queue in this runtime iteration.
		progressed = process_scheduler_jobs() || progressed;
		progressed = process_storage_completions() || progressed;
		if (has_pending_edit_operation()) {
			progressed = process_world_operation_event() || progressed;
		}
		progressed = page_runtime_->resume_loading_records(
			storage_,
			*page_cache_,
			*scheduler_,
			4
		) != 0 || progressed;
		progressed = process_async_mesh_completions() || progressed;
		progressed = process_deferred_gpu_captures() || progressed;
		progressed = page_runtime_->flush_scheduler_results(*scheduler_) != 0 ||
			progressed;
		progressed = scheduler_->apply_completions(
			static_cast<std::size_t>(config_.active_chunk_capacity)
		) != 0 || progressed;
		progressed = process_pending_transition_remeshes() || progressed;
		progressed = process_scheduler_jobs() || progressed;
		progressed = process_foreground_priority_event() || progressed;
		progressed = process_viewer_event() || progressed;
		progressed = process_async_mesh_completions() || progressed;
		progressed = process_deferred_gpu_captures() || progressed;
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
			const std::vector<WtChunkApplicationRecord> application_records =
				application_->get_records();
			const bool collision_repair_pending = std::any_of(
				application_records.begin(),
				application_records.end(),
				[](const WtChunkApplicationRecord &record) {
					return record.collision_required &&
						(!record.collision_ready ||
							record.collision_generation != record.generation);
				}
			);
			std::unique_lock<std::mutex> lock(wake_mutex_);
			const auto wake_predicate = [&]() {
				return stop_requested_.load() ||
					wake_sequence_ != observed_wake;
			};
			if (collision_repair_pending) {
				const bool signaled = wake_condition_.wait_for(
					lock,
					std::chrono::milliseconds(4),
					wake_predicate
				);
				if (!signaled) {
					std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
					++metrics_.collision_readiness_repair_timed_wakes;
				}
			} else {
				wake_condition_.wait(lock, wake_predicate);
			}
			observed_wake = wake_sequence_;
		}
	}
	if (gpu_meshing_shadow_) {
		gpu_meshing_shadow_->set_capacity_available_notifier({});
	}
	page_runtime_->set_mesh_completion_notifier({});
	storage_.set_completion_notifier({});
	if (edit_journal_store_ != nullptr &&
		edit_journal_store_->flush_deferred() !=
			WtEditJournalStoreStatus::Ok) {
		set_failure(WtReadOnlyRuntimeStatus::EditFailure);
	}
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
	const WtChunkKey trace_key = publication.key;
	const WtGenerationToken trace_generation = publication.generation;
	const std::uint64_t trace_revision = trace_enabled ?
		publication.world_revision : 0;
	const std::uint64_t trace_kind =
		static_cast<std::uint64_t>(publication.kind);
	const bool trace_interaction_critical = publication.interaction_critical;
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
			trace_kind,
			0,
			trace_interaction_critical ? 1 : 0
		);
	}
	{
		std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
		++metrics_.published_events;
	}
	if (trace_kind == static_cast<std::uint64_t>(
			WtReadOnlyPublicationKind::CollisionPayload
		)) {
		const bool already_pending = std::find_if(
			collision_readiness_repair_attempts_.begin(),
			collision_readiness_repair_attempts_.end(),
			[&](const CollisionReadinessRepairAttempt &attempt) {
				return attempt.key == trace_key &&
					attempt.generation == trace_generation;
			}
		) != collision_readiness_repair_attempts_.end();
		if (!already_pending) {
			collision_readiness_repair_attempts_.push_back({
				trace_key,
				trace_generation,
			});
		}
	}
	return true;
}

bool WtReadOnlyWorldRuntime::pop_publication(
	WtReadOnlyPublication &publication
) {
	std::lock_guard<std::mutex> lock(publication_mutex_);
	const auto pop_interaction_payload = [this, &publication](
		std::vector<WtReadOnlyPublication> &slots,
		std::size_t &head,
		std::size_t &count
	) {
		for (std::size_t offset = 0; offset < count; ++offset) {
			const std::size_t index = (head + offset) % slots.size();
			const WtReadOnlyPublication &candidate = slots[index];
			if (!candidate.interaction_critical ||
				(candidate.kind != WtReadOnlyPublicationKind::RenderPayload &&
					candidate.kind != WtReadOnlyPublicationKind::CollisionPayload)) {
				continue;
			}
			bool prerequisite_pending = false;
			for (std::size_t prior = 0; prior < offset; ++prior) {
				const WtReadOnlyPublication &predecessor =
					slots[(head + prior) % slots.size()];
				if (predecessor.key != candidate.key ||
					predecessor.generation != candidate.generation) {
					continue;
				}
				prerequisite_pending =
					predecessor.kind == WtReadOnlyPublicationKind::ExpectChunk ||
					(candidate.kind ==
						WtReadOnlyPublicationKind::CollisionPayload &&
					 predecessor.kind ==
						WtReadOnlyPublicationKind::SetCollisionRequired &&
					 predecessor.collision_required);
				if (prerequisite_pending) break;
			}
			if (prerequisite_pending) continue;
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
	bool interaction_priority = pop_interaction_payload(
		priority_publication_slots_, priority_publication_head_,
		priority_publication_count_
	);
	bool interaction_normal = !interaction_priority && pop_interaction_payload(
		publication_slots_, publication_head_, publication_count_
	);
	if (interaction_priority) {
		++priority_publication_burst_;
	} else if (interaction_normal) {
		priority_publication_burst_ = 0;
	}
	const bool pop_normal =
		!interaction_priority && !interaction_normal &&
		publication_count_ != 0 &&
		(priority_publication_count_ == 0 ||
			priority_publication_burst_ >= kWtPublicationPriorityBurstLimit);
	if (!interaction_priority && !interaction_normal) {
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

bool WtReadOnlyWorldRuntime::pop_interaction_collision_publication(
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
			const WtReadOnlyPublication &candidate = slots[index];
			if (!candidate.interaction_critical ||
					candidate.kind != WtReadOnlyPublicationKind::CollisionPayload) {
				continue;
			}
			bool prerequisite_pending = false;
			for (std::size_t prior = 0; prior < offset; ++prior) {
				const WtReadOnlyPublication &predecessor =
					slots[(head + prior) % slots.size()];
				if (predecessor.key != candidate.key ||
					predecessor.generation != candidate.generation) {
					continue;
				}
				prerequisite_pending =
					predecessor.kind == WtReadOnlyPublicationKind::ExpectChunk ||
					(predecessor.kind ==
						WtReadOnlyPublicationKind::SetCollisionRequired &&
					 predecessor.collision_required);
				if (prerequisite_pending) break;
			}
			if (prerequisite_pending) continue;
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
	const bool priority = pop_matching(
		priority_publication_slots_, priority_publication_head_,
		priority_publication_count_
	);
	if (!priority && !pop_matching(
			publication_slots_, publication_head_, publication_count_
		)) {
		return false;
	}
	priority_publication_burst_ = priority ?
		priority_publication_burst_ + 1U : 0U;
	publication_space_available_.notify_one();
	if (causal_trace_.enabled()) {
		causal_trace_.record(
			WtCausalTraceEventKind::PublicationPopped,
			WtCausalTraceThreadRole::Frontend,
			&publication.key,
			publication.generation,
			publication.world_revision,
			static_cast<std::uint64_t>(publication.kind)
		);
	}
	return true;
}

bool WtReadOnlyWorldRuntime::pop_interaction_gpu_placeholder_publication(
	const WtChunkKey &key,
	WtGenerationToken generation,
	WtReadOnlyPublication &publication
) {
	std::lock_guard<std::mutex> lock(publication_mutex_);
	const auto pop_matching = [&key, generation, &publication](
		std::vector<WtReadOnlyPublication> &slots,
		std::size_t &head,
		std::size_t &count
	) {
		for (std::size_t offset = 0; offset < count; ++offset) {
			const std::size_t index = (head + offset) % slots.size();
			const WtReadOnlyPublication &candidate = slots[index];
			if (candidate.key != key ||
				candidate.generation != generation ||
				candidate.kind != WtReadOnlyPublicationKind::RenderPayload ||
				!candidate.render || candidate.render->publication_source !=
					WtRenderPublicationSource::GpuResidentPlaceholder) {
				continue;
			}
			bool prerequisite_pending = false;
			for (std::size_t prior = 0; prior < offset; ++prior) {
				const WtReadOnlyPublication &predecessor =
					slots[(head + prior) % slots.size()];
				if (predecessor.key == key && predecessor.generation == generation &&
					predecessor.kind == WtReadOnlyPublicationKind::ExpectChunk) {
					prerequisite_pending = true;
					break;
				}
			}
			if (prerequisite_pending) continue;
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
	const bool priority = pop_matching(
		priority_publication_slots_, priority_publication_head_,
		priority_publication_count_
	);
	if (!priority && !pop_matching(
			publication_slots_, publication_head_, publication_count_
		)) {
		return false;
	}
	priority_publication_burst_ = priority ? priority_publication_burst_ + 1U : 0U;
	publication_space_available_.notify_one();
	if (causal_trace_.enabled()) {
		causal_trace_.record(
			WtCausalTraceEventKind::PublicationPopped,
			WtCausalTraceThreadRole::Frontend,
			&publication.key,
			publication.generation,
			publication.world_revision,
			static_cast<std::uint64_t>(publication.kind)
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
		case WtReadOnlyPublicationKind::ViewerPlanStarted:
		case WtReadOnlyPublicationKind::ViewerPlanCompleted:
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
	notify_work();
}

void WtReadOnlyWorldRuntime::notify_visual_activation(
	const WtChunkKey &key,
	WtGenerationToken generation
) noexcept {
	if (!wt_is_valid_chunk_key(key) || generation.value == 0) return;
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(visual_activation_mutex_);
		const auto iterator = std::lower_bound(
			visual_activations_.begin(), visual_activations_.end(), key,
			[](const VisualActivation &item, const WtChunkKey &value) {
				return item.key < value;
			}
		);
		if (iterator != visual_activations_.end() && iterator->key == key) {
			if (iterator->generation.value != generation.value) {
				iterator->generation = generation;
				changed = true;
			}
		} else {
			visual_activations_.insert(iterator, { key, generation });
			changed = true;
		}
		if (changed) {
			visual_activation_sequence_.fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (changed) notify_work();
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
	{
		std::lock_guard<std::mutex> lock(publication_mutex_);
		snapshot.pending_publication_events = publication_count_;
		snapshot.pending_priority_publication_events =
			priority_publication_count_;
	}
	{
		std::lock_guard<std::mutex> lock(input_mutex_);
		snapshot.pending_viewer_events = viewer_events_.size();
	}
	if (!planner_viewers_.empty()) {
		snapshot.committed_visual_viewer_revision =
			planner_viewers_.front().snapshot.revision;
		snapshot.committed_visual_viewer_position_x = static_cast<std::int64_t>(
			planner_viewers_.front().snapshot.x
		);
		snapshot.committed_visual_viewer_position_z = static_cast<std::int64_t>(
			planner_viewers_.front().snapshot.z
		);
	}
	if (!collision_viewers_.empty()) {
		snapshot.committed_collision_viewer_revision =
			collision_viewers_.front().snapshot.revision;
		snapshot.committed_collision_viewer_position_x =
			static_cast<std::int64_t>(collision_viewers_.front().snapshot.x);
		snapshot.committed_collision_viewer_position_z =
			static_cast<std::int64_t>(collision_viewers_.front().snapshot.z);
	}
	snapshot.collision_readiness_repair_attempt_count =
		collision_readiness_repair_attempts_.size();
	if (desired_) {
		for (const WtDesiredChunk &item : desired_->get_desired_chunks()) {
			if (item.collision_required) ++snapshot.desired_collision_chunks;
		}
	}
	snapshot.page_cache_encoded_entry_capacity =
		config_.encoded_page_entry_capacity;
	snapshot.page_cache_encoded_byte_capacity =
		config_.encoded_page_byte_capacity;
	snapshot.page_cache_decoded_entry_capacity =
		config_.decoded_page_entry_capacity;
	snapshot.page_cache_decoded_byte_capacity =
		config_.decoded_page_byte_capacity;
	snapshot.resource_cache_mesh_entry_capacity = config_.mesh_entry_capacity;
	snapshot.resource_cache_mesh_byte_capacity = config_.mesh_byte_capacity;
	snapshot.resource_cache_render_entry_capacity = config_.render_entry_capacity;
	snapshot.resource_cache_render_byte_capacity = config_.render_byte_capacity;
	snapshot.resource_cache_collision_entry_capacity =
		config_.collision_entry_capacity;
	snapshot.resource_cache_collision_byte_capacity =
		config_.collision_byte_capacity;
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
		snapshot.edit_exact_delta_chunks = edit.exact_delta_chunks;
		snapshot.edit_exact_delta_dirty_blocks =
			edit.exact_delta_dirty_blocks;
		snapshot.edit_maximum_dirty_blocks_per_chunk =
			edit.maximum_dirty_blocks_per_chunk;
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
	snapshot.storage_cancelled_queued_requests =
		storage.cancelled_queued_requests;
	snapshot.storage_interaction_cancelled_queued_requests =
		storage.interaction_cancelled_queued_requests;
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
		snapshot.page_gpu_resident_visual_only_completions =
			page.gpu_resident_visual_only_completions;
		snapshot.page_storage_failures = page.storage_failures;
		snapshot.page_cache_failures = page.cache_failures;
		snapshot.page_scheduler_backpressure = page.scheduler_backpressure;
		snapshot.page_dependency_requests = page.dependency_requests;
		snapshot.page_dependency_reprioritizations =
			page.dependency_reprioritizations;
		snapshot.page_cancelled_dependency_requests =
			page.cancelled_dependency_requests;
		snapshot.page_dependency_cache_hits = page.dependency_cache_hits;
		snapshot.page_dependency_cache_misses = page.dependency_cache_misses;
		snapshot.page_accepted_storage_completions =
			page.accepted_storage_completions;
		snapshot.page_stale_storage_completions =
			page.stale_storage_completions;
		snapshot.page_loading_records = page.loading_records;
		snapshot.page_sample_ready_records = page.sample_ready_records;
		snapshot.page_awaiting_mesh_records = page.awaiting_mesh_records;
		snapshot.page_meshing_records = page.meshing_records;
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
		snapshot.mesh_prepare_time_ns_last = page.mesh_prepare_time_ns_last;
		snapshot.mesh_prepare_time_ns_total = page.mesh_prepare_time_ns_total;
		snapshot.mesh_prepare_time_ns_maximum =
			page.mesh_prepare_time_ns_maximum;
		snapshot.mesh_completion_time_ns_last =
			page.mesh_completion_time_ns_last;
		snapshot.mesh_completion_time_ns_total =
			page.mesh_completion_time_ns_total;
		snapshot.mesh_completion_time_ns_maximum =
			page.mesh_completion_time_ns_maximum;
		snapshot.cumulative_dirty_mask_avoided =
			page.cumulative_dirty_mask_avoided;
		snapshot.mesh_worker_count = page.mesh_worker_count;
		snapshot.mesh_worker_accepted_jobs = page.mesh_worker_accepted_jobs;
		snapshot.mesh_worker_started_jobs = page.mesh_worker_started_jobs;
		snapshot.mesh_worker_completed_jobs = page.mesh_worker_completed_jobs;
		snapshot.mesh_worker_queue_rejections =
			page.mesh_worker_queue_rejections;
		snapshot.mesh_worker_cancelled_queued_jobs =
			page.mesh_worker_cancelled_queued_jobs;
		snapshot.mesh_worker_reprioritized_queued_jobs =
			page.mesh_worker_reprioritized_queued_jobs;
		snapshot.mesh_worker_queued_jobs = page.mesh_worker_queued_jobs;
		snapshot.mesh_worker_queued_completions =
			page.mesh_worker_queued_completions;
		snapshot.mesh_worker_active_jobs = page.mesh_worker_active_jobs;
		snapshot.mesh_worker_maximum_active_jobs =
			page.mesh_worker_maximum_active_jobs;
		snapshot.mesh_worker_interactive_lane_count =
			page.mesh_worker_interactive_lane_count;
		snapshot.mesh_worker_interactive_accepted_jobs =
			page.mesh_worker_interactive_accepted_jobs;
		snapshot.mesh_worker_interactive_started_jobs =
			page.mesh_worker_interactive_started_jobs;
		snapshot.mesh_worker_interactive_completed_jobs =
			page.mesh_worker_interactive_completed_jobs;
		snapshot.mesh_worker_interactive_queued_jobs =
			page.mesh_worker_interactive_queued_jobs;
		snapshot.mesh_worker_interactive_active_jobs =
			page.mesh_worker_interactive_active_jobs;
		snapshot.mesh_worker_interactive_queue_wait_ns_last =
			page.mesh_worker_interactive_queue_wait_ns_last;
		snapshot.mesh_worker_interactive_queue_wait_ns_total =
			page.mesh_worker_interactive_queue_wait_ns_total;
		snapshot.mesh_worker_interactive_queue_wait_ns_maximum =
			page.mesh_worker_interactive_queue_wait_ns_maximum;
		snapshot.mesh_worker_queue_wait_ns_last =
			page.mesh_worker_queue_wait_ns_last;
		snapshot.mesh_worker_queue_wait_ns_total =
			page.mesh_worker_queue_wait_ns_total;
		snapshot.mesh_worker_queue_wait_ns_maximum =
			page.mesh_worker_queue_wait_ns_maximum;
		if (page.mesh_worker_count != 0) {
			snapshot.mesh_job_time_ns_last =
				page.mesh_worker_execute_time_ns_last;
			snapshot.mesh_job_time_ns_total =
				page.mesh_worker_execute_time_ns_total;
			snapshot.mesh_job_time_ns_maximum =
				page.mesh_worker_execute_time_ns_maximum;
		}
	}
	if (page_cache_) {
		const WtStoragePageCacheMetrics cache = page_cache_->get_metrics();
		snapshot.page_cache_encoded_entries = page_cache_->encoded_entry_count();
		snapshot.page_cache_encoded_resident_bytes =
			page_cache_->encoded_resident_bytes();
		snapshot.page_cache_decoded_entries = page_cache_->decoded_entry_count();
		snapshot.page_cache_decoded_resident_bytes =
			page_cache_->decoded_resident_bytes();
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
	if (resource_cache_) {
		snapshot.resource_cache_mesh_entries =
			resource_cache_->mesh_entry_count();
		snapshot.resource_cache_mesh_resident_bytes =
			resource_cache_->mesh_resident_bytes();
		snapshot.resource_cache_render_entries =
			resource_cache_->render_entry_count();
		snapshot.resource_cache_render_resident_bytes =
			resource_cache_->render_resident_bytes();
		snapshot.resource_cache_collision_entries =
			resource_cache_->collision_entry_count();
		snapshot.resource_cache_collision_resident_bytes =
			resource_cache_->collision_resident_bytes();
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
		case WtReadOnlyRuntimeStatus::InvalidForegroundPriority:
			return "foreground priority lease is invalid";
		case WtReadOnlyRuntimeStatus::ForegroundPriorityQueueFull:
			return "foreground priority event queue is full";
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
