#include "services/wt_read_only_world_runtime.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "meshing/wt_chunk_mesher.h"
#include "physics/wt_collision_builder.h"
#include "render/wt_render_payload.h"
#include "services/wt_chunk_application.h"
#include "services/wt_chunk_resource_cache.h"
#include "services/wt_desired_set_runtime.h"
#include "services/wt_edit_runtime_replacement.h"
#include "services/wt_page_meshing_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_edit_journal_store.h"
#include "storage/wt_storage_page_cache.h"
#include "editing/wt_edit_spatial_index.h"
#include "streaming/wt_stream_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace world_transvoxel {
namespace {

bool valid_radius(std::uint32_t radius, std::uint64_t capacity) noexcept {
	const std::uint64_t width = static_cast<std::uint64_t>(radius) * 2U + 1U;
	return width <= capacity && width <= capacity / width &&
		width * width <= capacity / width;
}

} // namespace

WtReadOnlyRuntimeStatus WtReadOnlyWorldRuntime::delta_failure_status(
	WtDesiredSetRuntimeStatus status
) noexcept {
	switch (status) {
		case WtDesiredSetRuntimeStatus::Ok:
			return WtReadOnlyRuntimeStatus::Ok;
		case WtDesiredSetRuntimeStatus::ChangeCapacityExceeded:
			return WtReadOnlyRuntimeStatus::RuntimeDeltaChangeCapacityExceeded;
		case WtDesiredSetRuntimeStatus::RuntimeStateMismatch:
			return WtReadOnlyRuntimeStatus::RuntimeDeltaStateMismatch;
		case WtDesiredSetRuntimeStatus::RecordCapacityExceeded:
			return WtReadOnlyRuntimeStatus::RuntimeDeltaRecordCapacityExceeded;
		case WtDesiredSetRuntimeStatus::JobQueueCapacityExceeded:
			return WtReadOnlyRuntimeStatus::RuntimeDeltaJobQueueCapacityExceeded;
		case WtDesiredSetRuntimeStatus::SchedulerFailure:
			return WtReadOnlyRuntimeStatus::RuntimeDeltaSchedulerFailure;
		case WtDesiredSetRuntimeStatus::ApplicationFailure:
			return WtReadOnlyRuntimeStatus::RuntimeDeltaApplicationFailure;
		case WtDesiredSetRuntimeStatus::PageMeshingRuntimeFailure:
			return WtReadOnlyRuntimeStatus::RuntimeDeltaPageMeshingRuntimeFailure;
		case WtDesiredSetRuntimeStatus::InvalidConfiguration:
		case WtDesiredSetRuntimeStatus::InvalidDelta:
			return WtReadOnlyRuntimeStatus::RuntimeDeltaFailure;
	}
	return WtReadOnlyRuntimeStatus::RuntimeDeltaFailure;
}

WtReadOnlyWorldRuntime::WtReadOnlyWorldRuntime(
	WtRuntimeConfig config,
	WtAsyncStorageService &storage,
	WtEditJournalStore *edit_journal_store
) :
		config_(config),
		storage_(storage),
		edit_journal_store_(edit_journal_store) {
	if (wt_validate_runtime_config(config_) != WtRuntimeConfigStatus::Ok ||
		!storage_.is_open()) {
		last_status_.store(WtReadOnlyRuntimeStatus::InvalidConfiguration);
		return;
	}
	const std::size_t active = static_cast<std::size_t>(
		config_.active_chunk_capacity
	);
	const std::size_t viewers = static_cast<std::size_t>(config_.viewer_capacity);
	initial_world_revision_ = storage_.world_revision();
	world_revision_.store(
		edit_journal_store_ != nullptr && edit_journal_store_->is_open() ?
			edit_journal_store_->current_world_revision() :
			initial_world_revision_
	);
	desired_ = std::make_unique<WtMultiViewerDesiredSet>(
		WtMultiViewerDesiredSetLimits {
			1,
			active,
			active,
			active,
		}
	);
	lod_planner_ = std::make_unique<WtBalancedLodPlanner>(
		active,
		storage_.page_hierarchy(),
		static_cast<std::uint32_t>(config_.lod_refinement_radius_chunks),
		config_.global_coarse_lod_coverage
	);
	planner_viewers_.reserve(viewers);
	collision_viewers_.reserve(viewers);
	scheduler_ = std::make_unique<WtStreamScheduler>(
		active, active, active, viewers
	);
	application_ = std::make_unique<WtChunkApplicationService>(
		active, active, active
	);
	page_cache_ = std::make_unique<WtStoragePageCache>(
		WtStoragePageCacheLimits {
			static_cast<std::size_t>(config_.encoded_page_entry_capacity),
			static_cast<std::size_t>(config_.encoded_page_byte_capacity),
			static_cast<std::size_t>(config_.decoded_page_entry_capacity),
			static_cast<std::size_t>(config_.decoded_page_byte_capacity),
		}
	);
	resource_cache_ = std::make_unique<WtChunkResourceCache>(
		WtChunkResourceCacheLimits {
			static_cast<std::size_t>(config_.mesh_entry_capacity),
			static_cast<std::size_t>(config_.mesh_byte_capacity),
			static_cast<std::size_t>(config_.render_entry_capacity),
			static_cast<std::size_t>(config_.render_byte_capacity),
			static_cast<std::size_t>(config_.collision_entry_capacity),
			static_cast<std::size_t>(config_.collision_byte_capacity),
		}
	);
	desired_runtime_ = std::make_unique<WtDesiredSetRuntimeService>(
		std::min<std::size_t>(kWtMaximumDesiredChunkCount, active * 2U)
	);
	edit_spatial_index_ = std::make_unique<WtEditSpatialIndex>(
		active,
		kWtMaximumDesiredChunkCount,
		active
	);
	edit_replacement_ =
		std::make_unique<WtEditRuntimeReplacementService>(active);
	page_runtime_ = std::make_unique<WtPageMeshingRuntimeService>(active);
	mesher_ = std::make_unique<WtChunkMesher>(
		wt_get_transvoxel_mit_backend()
	);
	meshing_scratch_ = std::make_unique<WtChunkMeshingScratch>();
	viewer_event_capacity_ = std::max<std::size_t>(viewers * 2U, 2U);
	viewer_events_.reserve(viewer_event_capacity_);
	world_operation_capacity_ = kWtProductionWorldOperationCapacity;
	world_operations_.reserve(world_operation_capacity_);
	const std::size_t publication_capacity = std::max<std::size_t>(
		active * 4U,
		16U
	);
	priority_publication_slots_.resize(publication_capacity);
	publication_slots_.resize(publication_capacity);
	valid_ = desired_->valid() && lod_planner_->valid() && page_cache_->valid() &&
		resource_cache_->valid() && desired_runtime_->valid() &&
		edit_replacement_->valid() && page_runtime_->valid();
	if (!valid_) {
		last_status_.store(WtReadOnlyRuntimeStatus::InvalidConfiguration);
	}
}

WtReadOnlyWorldRuntime::~WtReadOnlyWorldRuntime() {
	storage_.set_trace_observer({});
	request_stop();
}

bool WtReadOnlyWorldRuntime::valid() const noexcept {
	return valid_;
}

bool WtReadOnlyWorldRuntime::begin_causal_trace() {
	if (!valid_ || !causal_trace_.begin(static_cast<std::size_t>(
			config_.trace_event_capacity
		))) {
		return false;
	}
	storage_.set_trace_observer(
		[this](
			WtAsyncStorageTraceEventKind kind,
			const WtChunkKey &key,
			WtGenerationToken generation,
			std::uint64_t duration_ns,
			WtPageLoadStatus status
		) {
			WtCausalTraceEventKind event_kind =
				WtCausalTraceEventKind::StorageRequested;
			if (kind == WtAsyncStorageTraceEventKind::Started) {
				event_kind = WtCausalTraceEventKind::StorageStarted;
			} else if (kind == WtAsyncStorageTraceEventKind::Finished) {
				event_kind = WtCausalTraceEventKind::StorageFinished;
			}
			causal_trace_.record(
				event_kind,
				WtCausalTraceThreadRole::Storage,
				&key,
				generation,
				0,
				0,
				duration_ns,
				static_cast<std::int64_t>(status)
			);
		}
	);
	return true;
}

void WtReadOnlyWorldRuntime::end_causal_trace() {
	storage_.set_trace_observer({});
	causal_trace_.end();
}

WtCausalTraceSnapshot WtReadOnlyWorldRuntime::causal_trace_snapshot(
	std::uint64_t first_sequence,
	std::size_t maximum_events
) const {
	return causal_trace_.snapshot(first_sequence, maximum_events);
}

void WtReadOnlyWorldRuntime::record_frontend_publication(
	const WtReadOnlyPublication &publication,
	std::int64_t status
) {
	causal_trace_.record(
		WtCausalTraceEventKind::FrontendPublicationProcessed,
		WtCausalTraceThreadRole::Frontend,
		static_cast<std::uint8_t>(publication.kind) <=
			static_cast<std::uint8_t>(
				WtReadOnlyPublicationKind::CollisionPayload
			) ? &publication.key : nullptr,
		publication.generation,
		publication.world_revision,
		static_cast<std::uint64_t>(publication.kind),
		0,
		status
	);
}

void WtReadOnlyWorldRuntime::record_frontend_sink(
	bool collision,
	const WtChunkKey &key,
	WtGenerationToken generation,
	std::uint64_t duration_ns,
	bool applied
) {
	causal_trace_.record(
		collision ? WtCausalTraceEventKind::CollisionSinkApplied :
			WtCausalTraceEventKind::RenderSinkApplied,
		WtCausalTraceThreadRole::Frontend,
		&key,
		generation,
		0,
		0,
		duration_ns,
		applied ? 0 : 1
	);
}

void WtReadOnlyWorldRuntime::record_frontend_visibility(
	WtCausalTraceEventKind kind,
	const WtChunkKey *key,
	WtGenerationToken generation,
	std::uint64_t cause_id,
	std::uint64_t auxiliary,
	std::int64_t status
) {
	causal_trace_.record(
		kind,
		WtCausalTraceThreadRole::Frontend,
		key,
		generation,
		cause_id,
		auxiliary,
		0,
		status
	);
}

WtReadOnlyRuntimeStatus WtReadOnlyWorldRuntime::update_viewer(
	const WtViewerSnapshot &snapshot,
	std::uint32_t radius_chunks,
	std::uint8_t maximum_lod
) {
	if (!valid_ || snapshot.id == 0 || snapshot.revision == 0 ||
		!std::isfinite(snapshot.x) || !std::isfinite(snapshot.y) ||
		!std::isfinite(snapshot.z) ||
		!valid_radius(radius_chunks, config_.demand_capacity_per_viewer) ||
		maximum_lod > kWtMaximumLod) {
		return WtReadOnlyRuntimeStatus::InvalidViewer;
	}
	return enqueue_viewer_event({
		ViewerEventKind::Update,
		snapshot,
		radius_chunks,
		maximum_lod,
	}) ? WtReadOnlyRuntimeStatus::Ok :
		WtReadOnlyRuntimeStatus::ViewerQueueFull;
}

WtReadOnlyRuntimeStatus WtReadOnlyWorldRuntime::remove_viewer(
	std::uint64_t viewer_id,
	std::uint64_t revision
) {
	if (!valid_ || viewer_id == 0 || revision == 0) {
		return WtReadOnlyRuntimeStatus::InvalidViewer;
	}
	WtViewerSnapshot snapshot;
	snapshot.id = viewer_id;
	snapshot.revision = revision;
	return enqueue_viewer_event({ ViewerEventKind::Remove, snapshot, 0, 0 }) ?
		WtReadOnlyRuntimeStatus::Ok :
		WtReadOnlyRuntimeStatus::ViewerQueueFull;
}

WtReadOnlyRuntimeStatus WtReadOnlyWorldRuntime::update_collision_viewer(
	const WtViewerSnapshot &snapshot,
	std::uint32_t radius_chunks
) {
	if (!valid_ || snapshot.id == 0 || snapshot.revision == 0 ||
		!std::isfinite(snapshot.x) || !std::isfinite(snapshot.y) ||
		!std::isfinite(snapshot.z) ||
		!valid_radius(radius_chunks, config_.demand_capacity_per_viewer)) {
		return WtReadOnlyRuntimeStatus::InvalidViewer;
	}
	return enqueue_viewer_event({
		ViewerEventKind::UpdateCollision,
		snapshot,
		radius_chunks,
		0,
	}) ? WtReadOnlyRuntimeStatus::Ok :
		WtReadOnlyRuntimeStatus::ViewerQueueFull;
}

WtReadOnlyRuntimeStatus WtReadOnlyWorldRuntime::remove_collision_viewer(
	std::uint64_t viewer_id,
	std::uint64_t revision
) {
	if (!valid_ || viewer_id == 0 || revision == 0) {
		return WtReadOnlyRuntimeStatus::InvalidViewer;
	}
	WtViewerSnapshot snapshot;
	snapshot.id = viewer_id;
	snapshot.revision = revision;
	return enqueue_viewer_event({
		ViewerEventKind::RemoveCollision,
		snapshot,
		0,
		0,
	}) ? WtReadOnlyRuntimeStatus::Ok :
		WtReadOnlyRuntimeStatus::ViewerQueueFull;
}

bool WtReadOnlyWorldRuntime::enqueue_viewer_event(
	const ViewerEvent &event
) {
	std::lock_guard<std::mutex> lock(input_mutex_);
	const auto existing = std::find_if(
		viewer_events_.begin(),
		viewer_events_.end(),
		[&](const ViewerEvent &queued) {
			const bool queued_collision =
				queued.kind == ViewerEventKind::UpdateCollision ||
				queued.kind == ViewerEventKind::RemoveCollision;
			const bool event_collision =
				event.kind == ViewerEventKind::UpdateCollision ||
				event.kind == ViewerEventKind::RemoveCollision;
			return queued_collision == event_collision &&
				queued.snapshot.id == event.snapshot.id;
		}
	);
	if (existing != viewer_events_.end()) {
		if (event.snapshot.revision <= existing->snapshot.revision) {
			return false;
		}
		*existing = event;
		{
			std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
			++metrics_.coalesced_viewer_events;
		}
		notify_work();
		return true;
	}
	if (viewer_events_.size() >= viewer_event_capacity_) return false;
	viewer_events_.push_back(event);
	notify_work();
	return true;
}
} // namespace world_transvoxel
