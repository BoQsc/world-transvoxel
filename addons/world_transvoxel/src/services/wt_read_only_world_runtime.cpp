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

const WtLodMapEntry *find_plan_entry(
	const std::vector<WtLodMapEntry> &entries,
	const WtChunkKey &key
) noexcept {
	const auto iterator = std::lower_bound(
		entries.begin(), entries.end(), key,
		[](const WtLodMapEntry &entry, const WtChunkKey &value) {
			return entry.key < value;
		}
	);
	return iterator != entries.end() && iterator->key == key ? &*iterator :
		nullptr;
}

} // namespace

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
		storage_.page_keys(),
		static_cast<std::uint32_t>(config_.lod_refinement_radius_chunks)
	);
	planner_viewers_.reserve(viewers);
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
	desired_runtime_ = std::make_unique<WtDesiredSetRuntimeService>(active);
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
	publication_slots_.resize(publication_capacity);
	valid_ = desired_->valid() && lod_planner_->valid() && page_cache_->valid() &&
		resource_cache_->valid() && desired_runtime_->valid() &&
		edit_replacement_->valid() && page_runtime_->valid();
	if (!valid_) {
		last_status_.store(WtReadOnlyRuntimeStatus::InvalidConfiguration);
	}
}

WtReadOnlyWorldRuntime::~WtReadOnlyWorldRuntime() {
	request_stop();
}

bool WtReadOnlyWorldRuntime::valid() const noexcept {
	return valid_;
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

bool WtReadOnlyWorldRuntime::enqueue_viewer_event(
	const ViewerEvent &event
) {
	std::lock_guard<std::mutex> lock(input_mutex_);
	const auto existing = std::find_if(
		viewer_events_.begin(),
		viewer_events_.end(),
		[&](const ViewerEvent &queued) {
			return queued.snapshot.id == event.snapshot.id;
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

bool WtReadOnlyWorldRuntime::process_viewer_event() {
	ViewerEvent event;
	{
		std::lock_guard<std::mutex> lock(input_mutex_);
		if (viewer_events_.empty()) return false;
		event = viewer_events_.front();
		viewer_events_.erase(viewer_events_.begin());
	}
	std::vector<WtLodPlannerViewer> candidate_viewers = planner_viewers_;
	const auto viewer = std::lower_bound(
		candidate_viewers.begin(), candidate_viewers.end(), event.snapshot.id,
		[](const WtLodPlannerViewer &item, std::uint64_t id) {
			return item.snapshot.id < id;
		}
	);
	if (event.kind == ViewerEventKind::Update) {
		if (viewer != candidate_viewers.end() &&
			viewer->snapshot.id == event.snapshot.id) {
			if (event.snapshot.revision <= viewer->snapshot.revision) {
				std::lock_guard<std::mutex> lock(metrics_mutex_);
				++metrics_.rejected_events;
				return true;
			}
			*viewer = {
				event.snapshot, event.radius_chunks, event.maximum_lod
			};
		} else if (candidate_viewers.size() >= config_.viewer_capacity) {
			std::lock_guard<std::mutex> lock(metrics_mutex_);
			++metrics_.rejected_events;
			return true;
		} else {
			candidate_viewers.insert(viewer, {
				event.snapshot, event.radius_chunks, event.maximum_lod
			});
		}
	} else {
		if (viewer == candidate_viewers.end() ||
			viewer->snapshot.id != event.snapshot.id ||
			event.snapshot.revision <= viewer->snapshot.revision) {
			std::lock_guard<std::mutex> lock(metrics_mutex_);
			++metrics_.rejected_events;
			return true;
		}
		candidate_viewers.erase(viewer);
	}

	const WtCollisionPolicy collision_policy {
		kWtDefaultCollisionThinRatioSquared,
		config_.collision_activation_distance,
		config_.collision_deactivation_distance,
	};
	WtBalancedLodPlan candidate_plan;
	if (lod_planner_->plan(
			candidate_viewers,
			desired_->get_desired_chunks(),
			collision_policy,
			candidate_plan
		) != WtBalancedLodPlannerStatus::Ok ||
		plan_revision_ == std::numeric_limits<std::uint64_t>::max()) {
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.rejected_events;
		return true;
	}

	WtMultiViewerDesiredSet candidate_desired = *desired_;
	WtDesiredSetDelta delta;
	WtViewerSnapshot plan_snapshot;
	plan_snapshot.id = 1;
	plan_snapshot.x = event.snapshot.x;
	plan_snapshot.y = event.snapshot.y;
	plan_snapshot.z = event.snapshot.z;
	plan_snapshot.revision = plan_revision_ + 1;
	if (candidate_desired.update_viewer(
			plan_snapshot, candidate_plan.demands, delta
		) != WtMultiViewerDesiredSetStatus::Ok) {
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.rejected_events;
		return true;
	}

	WtDesiredSetDelta transition_removals;
	for (const WtLodMapEntry &current : current_plan_.entries) {
		const WtLodMapEntry *next = find_plan_entry(
			candidate_plan.entries, current.key
		);
		if (next == nullptr ||
			next->transition_mask == current.transition_mask) continue;
		transition_removals.removed.push_back(current.key);
		const WtDesiredChunk *desired = candidate_desired.find_desired(
			current.key
		);
		if (desired == nullptr) {
			set_failure(WtReadOnlyRuntimeStatus::DesiredSetFailure);
			return true;
		}
		delta.added.push_back(*desired);
	}
	if (!transition_removals.removed.empty()) {
		delta.updated.erase(
			std::remove_if(
				delta.updated.begin(), delta.updated.end(),
				[&](const WtDesiredChunk &item) {
					return std::binary_search(
						transition_removals.removed.begin(),
						transition_removals.removed.end(),
						item.key
					);
				}
			),
			delta.updated.end()
		);
		std::sort(delta.added.begin(), delta.added.end(),
			[](const WtDesiredChunk &a, const WtDesiredChunk &b) {
				return a.key < b.key;
			}
		);
	}

	const auto apply_delta = [&](const WtDesiredSetDelta &change) {
		return desired_runtime_->apply_delta(
			change,
			storage_.source_revision(),
			world_revision_.load(),
			*scheduler_,
			*page_cache_,
			*resource_cache_,
			*application_,
			page_runtime_.get()
		) == WtDesiredSetRuntimeStatus::Ok;
	};
	if (!apply_delta(transition_removals) || !apply_delta(delta)) {
		set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
		return true;
	}
	if (!publish_delta(transition_removals) || !publish_delta(delta)) {
		if (!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
		return true;
	}
	const std::size_t planned_demand_count = candidate_plan.demands.size();
	*desired_ = std::move(candidate_desired);
	planner_viewers_ = std::move(candidate_viewers);
	current_plan_ = std::move(candidate_plan);
	plan_revision_ = plan_snapshot.revision;
	std::vector<WtChunkKey> active_keys;
	active_keys.reserve(current_plan_.entries.size());
	for (const WtLodMapEntry &entry : current_plan_.entries) {
		active_keys.push_back(entry.key);
	}
	if (edit_spatial_index_->rebuild(active_keys) != WtEditSpatialStatus::Ok) {
		set_failure(WtReadOnlyRuntimeStatus::EditFailure);
		return true;
	}
	for (const WtDesiredChunk &item : delta.updated) {
		if (!item.collision_required) continue;
		const WtChunkRecord *record = scheduler_->find_record(item.key);
		if (record == nullptr) continue;
		const auto collision = resource_cache_->find_collision(
			item.key,
			record->generation
		);
		if (collision && !push_publication({
				WtReadOnlyPublicationKind::CollisionPayload,
				collision->key,
				collision->generation,
				true,
				{},
				collision,
			})) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			return true;
		}
	}
	{
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		if (event.kind == ViewerEventKind::Update) {
			++metrics_.viewer_updates;
			metrics_.planned_demands += planned_demand_count;
		} else {
			++metrics_.viewer_removals;
		}
	}
	return true;
}

bool WtReadOnlyWorldRuntime::publish_delta(
	const WtDesiredSetDelta &delta
) {
	for (const WtChunkKey &key : delta.removed) {
		if (!push_publication({
				WtReadOnlyPublicationKind::RemoveChunk,
				key,
				{},
				false,
				{},
				{},
			})) return false;
	}
	for (const WtDesiredChunk &item : delta.updated) {
		const WtChunkRecord *record = scheduler_->find_record(item.key);
		if (record == nullptr || !push_publication({
				WtReadOnlyPublicationKind::SetCollisionRequired,
				item.key,
				record->generation,
				item.collision_required,
				{},
				{},
			})) return false;
	}
	for (const WtDesiredChunk &item : delta.added) {
		const WtChunkRecord *record = scheduler_->find_record(item.key);
		if (record == nullptr || !push_publication({
				WtReadOnlyPublicationKind::ExpectChunk,
				item.key,
				record->generation,
				item.collision_required,
				{},
				{},
			})) return false;
	}
	return true;
}

bool WtReadOnlyWorldRuntime::process_storage_completions() {
	bool progressed = false;
	WtPageLoadCompletion completion;
	while (storage_.pop_completion(completion)) {
		progressed = true;
		const WtPageMeshingRuntimeStatus status =
			page_runtime_->accept_storage_completion(
				completion,
				*page_cache_,
				*scheduler_
			);
		if (status != WtPageMeshingRuntimeStatus::Ok &&
			status != WtPageMeshingRuntimeStatus::CompletionNotOwned &&
			status != WtPageMeshingRuntimeStatus::StaleCompletion &&
			status != WtPageMeshingRuntimeStatus::SchedulerBackpressure &&
			status != WtPageMeshingRuntimeStatus::CacheFailure) {
			set_failure(WtReadOnlyRuntimeStatus::PipelineFailure);
			break;
		}
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.storage_completions;
	}
	return progressed;
}

bool WtReadOnlyWorldRuntime::process_scheduler_jobs() {
	bool progressed = false;
	WtChunkJob job;
	for (std::size_t count = 0; count < 4 && scheduler_->pop_job(job); ++count) {
		progressed = true;
		WtPageMeshingRuntimeStatus status;
		if (job.stage == WtChunkJobStage::Sample) {
			const WtLodMapEntry *entry = find_plan_entry(
				current_plan_.entries, job.key
			);
			if (entry == nullptr) {
				set_failure(WtReadOnlyRuntimeStatus::PipelineFailure);
				break;
			}
			status = page_runtime_->begin_sample_job(
				job,
				entry->transition_mask,
				storage_,
				*page_cache_,
				*scheduler_
			);
			std::lock_guard<std::mutex> lock(metrics_mutex_);
			++metrics_.sample_jobs;
		} else {
			status = page_runtime_->execute_mesh_job(
				job,
				*mesher_,
				*meshing_scratch_,
				*scheduler_,
				edit_journal_store_ != nullptr ?
					&edit_journal_store_->journal() : nullptr,
				initial_world_revision_
			);
			std::lock_guard<std::mutex> lock(metrics_mutex_);
			++metrics_.mesh_jobs;
		}
		if (status != WtPageMeshingRuntimeStatus::Ok &&
			status != WtPageMeshingRuntimeStatus::SchedulerBackpressure &&
			status != WtPageMeshingRuntimeStatus::StorageRequestFailure &&
			status != WtPageMeshingRuntimeStatus::CacheFailure &&
			status != WtPageMeshingRuntimeStatus::MeshingFailure &&
			status != WtPageMeshingRuntimeStatus::NotReady) {
			set_failure(WtReadOnlyRuntimeStatus::PipelineFailure);
			break;
		}
	}
	return progressed;
}

bool WtReadOnlyWorldRuntime::process_mesh_completions() {
	bool progressed = false;
	WtPageMeshCompletion completion;
	while (page_runtime_->pop_mesh_completion(completion)) {
		progressed = true;
		const WtChunkRecord *record = scheduler_->find_record(completion.key);
		if (record == nullptr || record->generation != completion.generation ||
			!completion.mesh) {
			continue;
		}
		auto render = std::make_shared<WtRenderPayload>();
		auto collision = std::make_shared<WtCollisionPayload>();
		const WtCollisionPolicy collision_policy {
			kWtDefaultCollisionThinRatioSquared,
			config_.collision_activation_distance,
			config_.collision_deactivation_distance,
		};
		if (resource_cache_->insert_mesh(
				completion.mesh,
				completion.generation,
				record->generation
			) != WtChunkResourceCacheStatus::Ok ||
			wt_build_render_payload(
				*completion.mesh,
				completion.generation,
				*render
			) != WtRenderBuildStatus::Ok ||
			wt_build_collision_payload(
				*render,
				collision_policy,
				*collision
			) != WtCollisionBuildStatus::Ok ||
			resource_cache_->insert_render(render, record->generation) !=
				WtChunkResourceCacheStatus::Ok ||
			resource_cache_->insert_collision(collision, record->generation) !=
				WtChunkResourceCacheStatus::Ok) {
			set_failure(WtReadOnlyRuntimeStatus::PipelineFailure);
			break;
		}
		if (!push_publication({
				WtReadOnlyPublicationKind::RenderPayload,
				render->key,
				render->generation,
				false,
				render,
				{},
			})) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			break;
		}
		const WtDesiredChunk *desired = desired_->find_desired(completion.key);
		if (desired != nullptr && desired->collision_required &&
			!push_publication({
				WtReadOnlyPublicationKind::CollisionPayload,
				collision->key,
				collision->generation,
				true,
				{},
				collision,
			})) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			break;
		}
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.mesh_completions;
		if (completion.mesh->transition_mask != 0) {
			++metrics_.transition_mesh_completions;
		}
	}
	return progressed;
}

} // namespace world_transvoxel
