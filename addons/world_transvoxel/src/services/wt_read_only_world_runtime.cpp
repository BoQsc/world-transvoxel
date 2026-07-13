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

constexpr std::size_t kWtEditLodRetentionCapacity = 32;
constexpr std::uint64_t kWtEditLodRetentionViewerIdBase =
	0x8000000000000000ULL;
constexpr std::uint32_t kWtEditLodRetentionRadiusChunks = 1;
constexpr double kWtEditLodRetentionMergeDistance = 32.0;
constexpr double kWtEditLodRetentionVisibilitySlackRoots = 1.0;

bool valid_radius(std::uint32_t radius, std::uint64_t capacity) noexcept {
	const std::uint64_t width = static_cast<std::uint64_t>(radius) * 2U + 1U;
	return width <= capacity && width <= capacity / width &&
		width * width <= capacity / width;
}

WtReadOnlyRuntimeStatus read_only_delta_failure_status(
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

double bounds_center_axis(
	std::int64_t minimum,
	std::int64_t maximum
) noexcept {
	return static_cast<double>(minimum) * 0.5 +
		static_cast<double>(maximum) * 0.5;
}

double squared_distance(
	double ax,
	double ay,
	double az,
	double bx,
	double by,
	double bz
) noexcept {
	const double dx = ax - bx;
	const double dy = ay - by;
	const double dz = az - bz;
	return dx * dx + dy * dy + dz * dz;
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

void WtReadOnlyWorldRuntime::remember_edit_lod_retention_zones(
	const WtEditTransaction &transaction
) {
	for (const WtEditCommand &command : transaction.commands) {
		EditLodRetentionZone zone;
		zone.x = bounds_center_axis(command.bounds.minimum.x,
			command.bounds.maximum.x);
		zone.y = bounds_center_axis(command.bounds.minimum.y,
			command.bounds.maximum.y);
		zone.z = bounds_center_axis(command.bounds.minimum.z,
			command.bounds.maximum.z);
		zone.revision = next_edit_lod_retention_revision_++;
		bool merged = false;
		const double merge_distance_squared =
			kWtEditLodRetentionMergeDistance *
			kWtEditLodRetentionMergeDistance;
		for (EditLodRetentionZone &existing : edit_lod_retention_zones_) {
			if (squared_distance(
					existing.x, existing.y, existing.z,
					zone.x, zone.y, zone.z
				) > merge_distance_squared) {
				continue;
			}
			existing.x = (existing.x + zone.x) * 0.5;
			existing.y = (existing.y + zone.y) * 0.5;
			existing.z = (existing.z + zone.z) * 0.5;
			existing.revision = zone.revision;
			merged = true;
			break;
		}
		if (merged) {
			continue;
		}
		if (edit_lod_retention_zones_.size() <
			kWtEditLodRetentionCapacity) {
			edit_lod_retention_zones_.push_back(zone);
			continue;
		}
		const auto oldest = std::min_element(
			edit_lod_retention_zones_.begin(),
			edit_lod_retention_zones_.end(),
			[](const EditLodRetentionZone &left,
				const EditLodRetentionZone &right) {
				return left.revision < right.revision;
			}
		);
		if (oldest != edit_lod_retention_zones_.end()) {
			*oldest = zone;
		}
	}
	std::lock_guard<std::mutex> lock(metrics_mutex_);
	metrics_.edit_lod_retention_zones = edit_lod_retention_zones_.size();
}

std::size_t WtReadOnlyWorldRuntime::append_edit_lod_retention_viewers(
	const std::vector<WtLodPlannerViewer> &real_viewers,
	std::vector<WtLodPlannerViewer> &planning_viewers
) const {
	if (real_viewers.empty() || edit_lod_retention_zones_.empty()) {
		return 0;
	}
	std::uint8_t maximum_lod = 0;
	for (const WtLodPlannerViewer &viewer : real_viewers) {
		maximum_lod = std::max(maximum_lod, viewer.maximum_lod);
	}
	std::size_t appended = 0;
	for (const EditLodRetentionZone &zone : edit_lod_retention_zones_) {
		bool visible_to_real_viewer = false;
		for (const WtLodPlannerViewer &viewer : real_viewers) {
			const double root_extent =
				static_cast<double>(wt_chunk_extent(viewer.maximum_lod));
			const double active_distance =
				(static_cast<double>(viewer.radius_chunks) +
					kWtEditLodRetentionVisibilitySlackRoots) * root_extent;
			if (std::abs(zone.x - viewer.snapshot.x) <= active_distance &&
				std::abs(zone.z - viewer.snapshot.z) <= active_distance) {
				visible_to_real_viewer = true;
				break;
			}
		}
		if (!visible_to_real_viewer) {
			continue;
		}
		planning_viewers.push_back({
			{
				kWtEditLodRetentionViewerIdBase +
					static_cast<std::uint64_t>(appended) + 1ULL,
				zone.x,
				zone.y,
				zone.z,
				zone.revision,
			},
			kWtEditLodRetentionRadiusChunks,
			maximum_lod,
		});
		++appended;
	}
	return appended;
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
	std::vector<WtLodPlannerViewer> planning_viewers = candidate_viewers;
	std::size_t edit_retention_viewers =
		append_edit_lod_retention_viewers(candidate_viewers, planning_viewers);
	bool edit_retention_fallback = false;
	WtBalancedLodPlan candidate_plan;
	WtBalancedLodPlannerStatus plan_status = lod_planner_->plan(
			planning_viewers,
			desired_->get_desired_chunks(),
			collision_policy,
			candidate_plan
		);
	if (plan_status != WtBalancedLodPlannerStatus::Ok &&
			edit_retention_viewers != 0) {
		edit_retention_viewers = 0;
		edit_retention_fallback = true;
		planning_viewers = candidate_viewers;
		candidate_plan.clear();
		plan_status = lod_planner_->plan(
			planning_viewers,
			desired_->get_desired_chunks(),
			collision_policy,
			candidate_plan
		);
	}
	if (plan_status != WtBalancedLodPlannerStatus::Ok ||
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

	std::vector<WtDesiredChunk> transition_remeshes;
	for (const WtLodMapEntry &current : current_plan_.entries) {
		const WtLodMapEntry *next = find_plan_entry(
			candidate_plan.entries, current.key
		);
		if (next == nullptr ||
			next->transition_mask == current.transition_mask) continue;
		const WtDesiredChunk *desired = candidate_desired.find_desired(
			current.key
		);
		if (desired == nullptr) {
			set_failure(WtReadOnlyRuntimeStatus::DesiredSetFailure);
			return true;
		}
		transition_remeshes.push_back(*desired);
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
		);
	};
	const WtDesiredSetRuntimeStatus delta_status = apply_delta(delta);
	if (delta_status == WtDesiredSetRuntimeStatus::JobQueueCapacityExceeded &&
		scheduler_->queued_job_count() != 0) {
		std::lock_guard<std::mutex> lock(input_mutex_);
		viewer_events_.insert(viewer_events_.begin(), event);
		return true;
	}
	if (delta_status != WtDesiredSetRuntimeStatus::Ok) {
		set_failure(read_only_delta_failure_status(delta_status));
		return true;
	}
	if (!publish_delta(delta)) {
		if (!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
		return true;
	}
	const std::size_t planned_demand_count = candidate_plan.demands.size();
	*desired_ = std::move(candidate_desired);
	planner_viewers_ = std::move(candidate_viewers);
	current_plan_ = std::move(candidate_plan);
	queue_transition_remeshes(transition_remeshes);
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
		metrics_.edit_lod_retention_zones =
			edit_lod_retention_zones_.size();
		metrics_.edit_lod_retention_active_viewers =
			edit_retention_viewers;
		if (edit_retention_fallback) {
			++metrics_.edit_lod_retention_fallbacks;
		}
		if (edit_retention_viewers != 0) {
			++metrics_.edit_lod_retention_plans;
		}
	}
	process_pending_transition_remeshes();
	return true;
}

void WtReadOnlyWorldRuntime::queue_transition_remeshes(
	const std::vector<WtDesiredChunk> &chunks
) {
	for (const WtDesiredChunk &chunk : chunks) {
		const auto position = std::lower_bound(
			pending_transition_remeshes_.begin(),
			pending_transition_remeshes_.end(),
			chunk.key,
			[](const WtDesiredChunk &item, const WtChunkKey &key) {
				return item.key < key;
			}
		);
		if (position != pending_transition_remeshes_.end() &&
			position->key == chunk.key) {
			*position = chunk;
		} else {
			pending_transition_remeshes_.insert(position, chunk);
		}
	}
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

bool WtReadOnlyWorldRuntime::process_pending_transition_remeshes() {
	bool progressed = false;
	for (std::size_t index = 0; index < pending_transition_remeshes_.size();) {
		if (scheduler_->available_job_capacity() == 0) {
			break;
		}
		const WtDesiredChunk item = pending_transition_remeshes_[index];
		const WtDesiredChunk *desired = desired_->find_desired(item.key);
		if (desired == nullptr ||
			find_plan_entry(current_plan_.entries, item.key) == nullptr) {
			pending_transition_remeshes_.erase(
				pending_transition_remeshes_.begin() + index
			);
			progressed = true;
			continue;
		}
		const WtChunkRecord *record = scheduler_->find_record(item.key);
		if (record == nullptr) {
			pending_transition_remeshes_.erase(
				pending_transition_remeshes_.begin() + index
			);
			progressed = true;
			continue;
		}
		if (record->lifecycle != WtChunkLifecycle::Ready) {
			++index;
			continue;
		}
		const WtSchedulerStatus scheduler_status =
			scheduler_->request_chunk_version(
				item.key,
				storage_.source_revision(),
				world_revision_.load(),
				desired->priority,
				true
			);
		if (scheduler_status == WtSchedulerStatus::JobQueueFull) {
			break;
		}
		if (scheduler_status != WtSchedulerStatus::Ok) {
			set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
			return true;
		}
		record = scheduler_->find_record(item.key);
		const WtApplicationStatus application_status =
			record == nullptr ? WtApplicationStatus::NotFound :
			application_->expect_chunk(
				item.key,
				record->generation,
				desired->collision_required
			);
		if (application_status != WtApplicationStatus::Ok &&
			application_status != WtApplicationStatus::AlreadyCurrent) {
			set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
			return true;
		}
		if (!push_publication({
				WtReadOnlyPublicationKind::ExpectChunk,
				item.key,
				record->generation,
				desired->collision_required,
				{},
				{},
			})) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			return true;
		}
		pending_transition_remeshes_.erase(
			pending_transition_remeshes_.begin() + index
		);
		progressed = true;
	}
	return progressed;
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
