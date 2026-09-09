#include "services/wt_read_only_world_runtime.h"

#include "services/wt_chunk_application.h"
#include "services/wt_chunk_resource_cache.h"
#include "services/wt_edit_runtime_replacement.h"
#include "services/wt_page_meshing_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_edit_journal_store.h"
#include "storage/wt_storage_page_cache.h"
#include "streaming/wt_stream_scheduler.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace world_transvoxel {
namespace {

WtReadOnlyEditStatus map_prepare_status(
	WtEditRuntimeReplacementStatus status
) noexcept {
	return status == WtEditRuntimeReplacementStatus::SpatialQueryFailed ?
		WtReadOnlyEditStatus::SpatialFailure :
		WtReadOnlyEditStatus::ReplacementFailure;
}

} // namespace

WtReadOnlyRuntimeStatus WtReadOnlyWorldRuntime::submit_edit(
	const WtEditTransaction &transaction
) {
	std::vector<std::uint8_t> encoded;
	if (!valid_ || edit_journal_store_ == nullptr ||
		wt_write_edit_transaction(transaction, encoded) !=
			WtEditTransactionStatus::Ok) {
		return WtReadOnlyRuntimeStatus::InvalidEdit;
	}
	WorldOperation operation;
	operation.kind = WorldOperationKind::Edit;
	operation.transaction = transaction;
	const bool accepted = enqueue_world_operation(operation);
	causal_trace_.record(
		WtCausalTraceEventKind::EditSubmitted,
		WtCausalTraceThreadRole::Api,
		nullptr,
		{},
		transaction.committed_revision,
		transaction.commands.size(),
		0,
		accepted ? 0 : static_cast<std::int64_t>(
			WtReadOnlyRuntimeStatus::EditQueueFull
		)
	);
	return accepted ? WtReadOnlyRuntimeStatus::Ok :
		WtReadOnlyRuntimeStatus::EditQueueFull;
}

bool WtReadOnlyWorldRuntime::process_edit_operation(
	WtEditTransaction transaction,
	bool rebase_queued_edit
) {
	const std::uint64_t current_revision = world_revision_.load();
	if (rebase_queued_edit && transaction.base_revision != current_revision &&
		!wt_rebase_edit_transaction(transaction, current_revision)) {
		rebase_queued_edit = false;
	}
	causal_trace_.record(
		WtCausalTraceEventKind::EditProcessingStarted,
		WtCausalTraceThreadRole::Runtime,
		nullptr,
		{},
		transaction.committed_revision,
		transaction.commands.size()
	);
	const auto reject = [&](WtReadOnlyEditStatus status) {
		causal_trace_.record(
			WtCausalTraceEventKind::EditRejected,
			WtCausalTraceThreadRole::Runtime,
			nullptr,
			{},
			transaction.committed_revision,
			0,
			0,
			static_cast<std::int64_t>(status)
		);
		{
			std::lock_guard<std::mutex> lock(metrics_mutex_);
			++metrics_.edit_rejections;
		}
		return push_publication({
			WtReadOnlyPublicationKind::EditRejected,
			{},
			{},
			false,
			{},
			{},
			world_revision_.load(),
			status,
		});
	};
	if (edit_journal_store_ == nullptr || !edit_journal_store_->is_open()) {
		if (!reject(WtReadOnlyEditStatus::JournalFailure) &&
			!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
		return true;
	}
	if (transaction.source_revision != storage_.source_revision() ||
		transaction.base_revision != world_revision_.load() ||
		transaction.committed_revision != transaction.base_revision + 1) {
		if (!reject(WtReadOnlyEditStatus::StaleRevision) &&
			!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
		return true;
	}
	const std::vector<WtDesiredChunk> *desired_chunks =
		desired_ ? &desired_->get_desired_chunks() : nullptr;
	const WtEditRuntimeReplacementStatus prepare =
		edit_replacement_->prepare_loaded_chunks(
			transaction,
			*edit_spatial_index_,
			*scheduler_,
			*application_,
			desired_chunks
		);
	if (prepare != WtEditRuntimeReplacementStatus::Ok) {
		if (!reject(map_prepare_status(prepare)) &&
			!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
		return true;
	}
	if (edit_journal_store_->append_deferred(transaction) !=
		WtEditJournalStoreStatus::Ok) {
		if (!reject(WtReadOnlyEditStatus::JournalFailure) &&
			!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
		return true;
	}
	world_revision_.store(transaction.committed_revision);
	causal_trace_.record(
		WtCausalTraceEventKind::EditJournalCommitted,
		WtCausalTraceThreadRole::Runtime,
		nullptr,
		{},
		transaction.committed_revision,
		transaction.commands.size()
	);
	remember_edit_lod_retention_zones(transaction);
	if (edit_replacement_->apply_prepared(
			transaction,
			*scheduler_,
			*page_cache_,
			*resource_cache_,
			*application_,
			page_runtime_.get()
		) != WtEditRuntimeReplacementStatus::Ok) {
		set_failure(WtReadOnlyRuntimeStatus::EditFailure);
		return true;
	}
	for (const WtEditRuntimeReplacementRecord &replacement :
			edit_replacement_->get_last_replacements()) {
		causal_trace_.record(
			WtCausalTraceEventKind::ChunkDemandAccepted,
			WtCausalTraceThreadRole::Runtime,
			&replacement.key,
			replacement.replacement_generation,
			transaction.committed_revision,
			1
		);
		causal_trace_.record(
			WtCausalTraceEventKind::EditDirtyPageAdmitted,
			WtCausalTraceThreadRole::Runtime,
			&replacement.key,
			replacement.replacement_generation,
			transaction.committed_revision,
			(replacement.visual_required ? 1U : 0U) |
				(replacement.collision_required ? 2U : 0U)
		);
		WtReadOnlyPublication publication;
		publication.kind = WtReadOnlyPublicationKind::ExpectChunk;
		publication.key = replacement.key;
		publication.generation = replacement.replacement_generation;
		publication.world_revision = transaction.committed_revision;
		publication.collision_required = replacement.collision_required;
		publication.visual_required = replacement.visual_required;
		publication.staged_replacement = true;
		publication.preserve_collision_ready = replacement.collision_required;
		publication.independently_publishable_replacement = true;
		if (!config_.hierarchical_lod_viewer_activation_enabled &&
			replacement.visual_required && replacement.key.lod != 0) {
			std::lock_guard<std::mutex> lock(visual_activation_mutex_);
			const auto visible = std::find_if(visual_activations_.begin(), visual_activations_.end(),
				[&](const VisualActivation &entry) { return entry.key == replacement.key; });
			if (visible != visual_activations_.end()) {
				const auto waiting = std::find_if(edit_content_activation_waits_.begin(),
					edit_content_activation_waits_.end(), [&](const VisualActivation &entry) {
						return entry.key == replacement.key;
					});
				if (waiting == edit_content_activation_waits_.end()) {
					edit_content_activation_waits_.push_back({replacement.key, replacement.replacement_generation});
				} else {
					waiting->generation = replacement.replacement_generation;
				}
			}
		}
		if (!push_publication(std::move(publication))) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			return true;
		}
		queue_readiness_repair_candidate(replacement.key);
	}
	if (!push_publication({
			WtReadOnlyPublicationKind::EditCommitted,
			{},
			{},
			false,
			{},
			{},
			transaction.committed_revision,
			WtReadOnlyEditStatus::Ok,
		})) {
		if (!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
		return true;
	}
	{
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.edit_commits;
		metrics_.edit_replacements +=
			edit_replacement_->get_last_replacements().size();
	}
	{
		std::lock_guard<std::mutex> lock(input_mutex_);
		edit_lod_retention_refresh_pending_ = true;
	}
	notify_work();
	causal_trace_.record(
		WtCausalTraceEventKind::EditCommitted,
		WtCausalTraceThreadRole::Runtime,
		nullptr,
		{},
		transaction.committed_revision,
		edit_replacement_->get_last_replacements().size()
	);
	return true;
}

} // namespace world_transvoxel
