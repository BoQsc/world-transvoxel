#include "services/wt_read_only_world_runtime.h"

#include "services/wt_desired_set_runtime.h"
#include "services/wt_page_meshing_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "streaming/wt_stream_scheduler.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace world_transvoxel {

bool WtReadOnlyWorldRuntime::process_foreground_priority_event() {
	ForegroundPriorityEvent event;
	{
		std::lock_guard<std::mutex> lock(input_mutex_);
		if (foreground_priority_events_.empty()) return false;
		event = std::move(foreground_priority_events_.front());
		foreground_priority_events_.erase(
			foreground_priority_events_.begin()
		);
	}

	WtForegroundPriorityLeaseSet candidate_leases =
		foreground_priority_leases_;
	const WtForegroundPriorityStatus lease_status =
		candidate_leases.update(event.request);
	if (lease_status == WtForegroundPriorityStatus::StaleRevision) {
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.foreground_priority_stale_events;
		causal_trace_.record(
			WtCausalTraceEventKind::ForegroundPriorityLeaseRejected,
			WtCausalTraceThreadRole::Runtime,
			nullptr,
			{},
			event.request.source_id,
			event.request.revision,
			0,
			static_cast<std::int64_t>(lease_status)
		);
		return true;
	}
	if (lease_status != WtForegroundPriorityStatus::Ok) {
		set_failure(WtReadOnlyRuntimeStatus::InvalidForegroundPriority);
		return true;
	}

	std::vector<WtViewerChunkDemand> effective_demands;
	const WtForegroundPriorityOverlayResult overlay = candidate_leases.apply(
		base_demands_,
		effective_demands
	);
	WtDesiredSetDelta delta;
	WtMultiViewerDesiredSet candidate_desired = *desired_;
	if (!base_demands_.empty()) {
		if (plan_revision_ == std::numeric_limits<std::uint64_t>::max()) {
			set_failure(WtReadOnlyRuntimeStatus::DesiredSetFailure);
			return true;
		}
		WtViewerSnapshot plan_snapshot;
		plan_snapshot.id = 1;
		plan_snapshot.revision = plan_revision_ + 1;
		if (candidate_desired.update_viewer(
				plan_snapshot,
				effective_demands,
				delta
			) != WtMultiViewerDesiredSetStatus::Ok ||
			!delta.added.empty() || !delta.removed.empty() ||
			std::any_of(
				delta.updated.begin(),
				delta.updated.end(),
				[this](const WtDesiredChunk &item) {
					const WtDesiredChunk *current =
						desired_->find_desired(item.key);
					return current == nullptr ||
						current->supporter_count != item.supporter_count ||
						current->collision_required !=
							item.collision_required ||
						current->visual_required != item.visual_required;
				}
			)) {
			set_failure(WtReadOnlyRuntimeStatus::DesiredSetFailure);
			return true;
		}
		const WtDesiredSetRuntimeStatus delta_status =
			desired_runtime_->apply_delta(
				delta,
				storage_.source_revision(),
				world_revision_.load(),
				*scheduler_,
				*page_cache_,
				*resource_cache_,
				*application_,
				page_runtime_.get()
			);
		if (delta_status != WtDesiredSetRuntimeStatus::Ok) {
			set_failure(delta_failure_status(delta_status));
			return true;
		}
		*desired_ = std::move(candidate_desired);
		plan_revision_ = plan_snapshot.revision;
		for (const WtDesiredChunk &item : delta.updated) {
			const WtChunkRecord *record = scheduler_->find_record(item.key);
			causal_trace_.record(
				WtCausalTraceEventKind::ForegroundPriorityChanged,
				WtCausalTraceThreadRole::Runtime,
				&item.key,
				record == nullptr ? WtGenerationToken{} : record->generation,
				event.request.source_id,
				event.request.revision,
				0,
				item.priority
			);
		}
	}

	foreground_priority_leases_ = std::move(candidate_leases);
	const bool release = event.request.keys.empty();
	causal_trace_.record(
		WtCausalTraceEventKind::ForegroundPriorityLeaseApplied,
		WtCausalTraceThreadRole::Runtime,
		nullptr,
		{},
		event.request.source_id,
		event.request.revision,
		0,
		static_cast<std::int64_t>(event.request.priority_class)
	);
	{
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		if (release) {
			++metrics_.foreground_priority_releases;
		} else {
			++metrics_.foreground_priority_updates;
		}
		metrics_.foreground_priority_active_sources =
			foreground_priority_leases_.active_source_count();
		metrics_.foreground_priority_support_keys =
			foreground_priority_leases_.active_key_count(
				WtForegroundPriorityClass::PlayerSupport
			);
		metrics_.foreground_priority_focus_keys =
			foreground_priority_leases_.active_key_count(
				WtForegroundPriorityClass::InteractionFocus
			);
		metrics_.foreground_priority_requested_keys +=
			overlay.requested_keys;
		metrics_.foreground_priority_matched_keys += overlay.matched_keys;
		metrics_.foreground_priority_missing_keys += overlay.missing_keys;
		metrics_.foreground_priority_changed_priorities +=
			overlay.changed_priorities;
		metrics_.foreground_priority_last_source_id = event.request.source_id;
		metrics_.foreground_priority_last_revision = event.request.revision;
		metrics_.foreground_priority_last_class =
			static_cast<std::uint64_t>(event.request.priority_class);
		metrics_.foreground_priority_last_effective_priority =
			wt_foreground_priority(event.request.priority_class);
		if (!event.request.keys.empty()) {
			const WtChunkKey &key = event.request.keys.front();
			metrics_.foreground_priority_last_key_x = key.x;
			metrics_.foreground_priority_last_key_y = key.y;
			metrics_.foreground_priority_last_key_z = key.z;
			metrics_.foreground_priority_last_key_lod = key.lod;
		}
	}
	return true;
}

} // namespace world_transvoxel
