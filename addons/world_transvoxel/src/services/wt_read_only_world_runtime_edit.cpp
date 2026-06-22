#include "services/wt_read_only_world_runtime.h"

#include "services/wt_chunk_application.h"
#include "services/wt_chunk_resource_cache.h"
#include "services/wt_edit_runtime_replacement.h"
#include "services/wt_page_meshing_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_edit_journal_store.h"
#include "storage/wt_storage_page_cache.h"
#include "streaming/wt_stream_scheduler.h"

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
	return enqueue_world_operation(operation) ?
		WtReadOnlyRuntimeStatus::Ok :
		WtReadOnlyRuntimeStatus::EditQueueFull;
}

bool WtReadOnlyWorldRuntime::process_edit_operation(
	const WtEditTransaction &transaction
) {
	const auto reject = [&](WtReadOnlyEditStatus status) {
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
	const WtEditRuntimeReplacementStatus prepare =
		edit_replacement_->prepare_loaded_chunks(
			transaction,
			*edit_spatial_index_,
			*scheduler_,
			*application_
		);
	if (prepare != WtEditRuntimeReplacementStatus::Ok) {
		if (!reject(map_prepare_status(prepare)) &&
			!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
		return true;
	}
	if (edit_journal_store_->append(transaction) !=
		WtEditJournalStoreStatus::Ok) {
		if (!reject(WtReadOnlyEditStatus::JournalFailure) &&
			!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
		return true;
	}
	world_revision_.store(transaction.committed_revision);
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
		if (!push_publication({
				WtReadOnlyPublicationKind::ExpectChunk,
				replacement.key,
				replacement.replacement_generation,
				replacement.collision_required,
				{},
				{},
			})) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			return true;
		}
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
	return true;
}

} // namespace world_transvoxel
