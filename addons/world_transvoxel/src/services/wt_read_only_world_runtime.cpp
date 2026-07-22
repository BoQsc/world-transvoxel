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

constexpr std::size_t kWtEditLodRetentionCapacity = 256;
constexpr std::uint64_t kWtEditLodRetentionViewerIdBase =
	0x8000000000000000ULL;
constexpr std::uint64_t kWtEditLodRetentionViewerIdMaximum =
	0xFFFFFFFFFFFFFFFFULL - kWtEditLodRetentionViewerIdBase;
constexpr std::uint32_t kWtEditLodRetentionRootRadiusChunks = 1;
constexpr std::uint32_t kWtEditLodRetentionMinimumRefinementRadiusChunks = 1;
constexpr std::uint32_t kWtEditLodRetentionMaximumRefinementRadiusChunks = 6;
constexpr std::uint32_t kWtEditLodRetentionRefinementMarginChunks = 1;
constexpr double kWtEditLodRetentionMergeDistance = 64.0;
constexpr double kWtEditLodRetentionVisibilitySlackRoots = 1.0;
constexpr std::size_t kWtEditLodRetentionAlwaysActiveRecentZones = 32;
constexpr std::int32_t kWtCollisionInvokerPriorityMaximum =
	std::numeric_limits<std::int32_t>::max();

bool valid_radius(std::uint32_t radius, std::uint64_t capacity) noexcept {
	const std::uint64_t width = static_cast<std::uint64_t>(radius) * 2U + 1U;
	return width <= capacity && width <= capacity / width &&
		width * width <= capacity / width;
}

bool chunk_coordinate(double position, std::int32_t &coordinate) noexcept {
	if (!std::isfinite(position)) return false;
	const double value = std::floor(
		position / static_cast<double>(wt_chunk_extent(0))
	);
	if (value < std::numeric_limits<std::int32_t>::min() ||
		value > std::numeric_limits<std::int32_t>::max()) {
		return false;
	}
	coordinate = static_cast<std::int32_t>(value);
	return true;
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

double interval_distance(
	std::int64_t a_minimum,
	std::int64_t a_maximum,
	std::int64_t b_minimum,
	std::int64_t b_maximum
) noexcept {
	if (a_maximum < b_minimum) {
		return static_cast<double>(b_minimum - a_maximum);
	}
	if (b_maximum < a_minimum) {
		return static_cast<double>(a_minimum - b_maximum);
	}
	return 0.0;
}

double point_interval_distance(
	double point,
	std::int64_t minimum,
	std::int64_t maximum
) noexcept {
	if (point < static_cast<double>(minimum)) {
		return static_cast<double>(minimum) - point;
	}
	if (point > static_cast<double>(maximum)) {
		return point - static_cast<double>(maximum);
	}
	return 0.0;
}

double bounds_distance_squared(
	const WtEditBounds &a,
	const WtEditBounds &b
) noexcept {
	const double dx = interval_distance(
		a.minimum.x, a.maximum.x, b.minimum.x, b.maximum.x
	);
	const double dy = interval_distance(
		a.minimum.y, a.maximum.y, b.minimum.y, b.maximum.y
	);
	const double dz = interval_distance(
		a.minimum.z, a.maximum.z, b.minimum.z, b.maximum.z
	);
	return dx * dx + dy * dy + dz * dz;
}

std::uint32_t edit_lod_retention_unbounded_refinement_radius(
	const WtGridPoint &minimum,
	const WtGridPoint &maximum
) noexcept {
	const double half_x = std::abs(bounds_center_axis(minimum.x, maximum.x) -
		static_cast<double>(minimum.x));
	const double half_y = std::abs(bounds_center_axis(minimum.y, maximum.y) -
		static_cast<double>(minimum.y));
	const double half_z = std::abs(bounds_center_axis(minimum.z, maximum.z) -
		static_cast<double>(minimum.z));
	const double half_extent = std::max({ half_x, half_y, half_z });
	const double lod0_extent = static_cast<double>(wt_chunk_extent(0));
	return static_cast<std::uint32_t>(
		std::ceil(half_extent / lod0_extent)
	) + kWtEditLodRetentionRefinementMarginChunks;
}

std::uint32_t edit_lod_retention_refinement_radius(
	const WtGridPoint &minimum,
	const WtGridPoint &maximum
) noexcept {
	return std::clamp(
		edit_lod_retention_unbounded_refinement_radius(minimum, maximum),
		kWtEditLodRetentionMinimumRefinementRadiusChunks,
		kWtEditLodRetentionMaximumRefinementRadiusChunks
	);
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
	page_keys_ = storage_.page_keys();
	std::sort(page_keys_.begin(), page_keys_.end());
	lod_planner_ = std::make_unique<WtBalancedLodPlanner>(
		active,
		page_keys_,
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

void WtReadOnlyWorldRuntime::remember_edit_lod_retention_zones(
	const WtEditTransaction &transaction
) {
	for (const WtEditCommand &command : transaction.commands) {
		EditLodRetentionZone zone;
		zone.minimum = command.bounds.minimum;
		zone.maximum = command.bounds.maximum;
		zone.x = bounds_center_axis(zone.minimum.x, zone.maximum.x);
		zone.y = bounds_center_axis(zone.minimum.y, zone.maximum.y);
		zone.z = bounds_center_axis(zone.minimum.z, zone.maximum.z);
		zone.refinement_radius_chunks =
			edit_lod_retention_refinement_radius(zone.minimum, zone.maximum);
		zone.revision = next_edit_lod_retention_revision_++;
		zone.viewer_id = kWtEditLodRetentionViewerIdBase +
			next_edit_lod_retention_viewer_id_;
		if (next_edit_lod_retention_viewer_id_ <
				kWtEditLodRetentionViewerIdMaximum) {
			++next_edit_lod_retention_viewer_id_;
		}
		bool merged = false;
		const double merge_distance_squared =
			kWtEditLodRetentionMergeDistance *
			kWtEditLodRetentionMergeDistance;
		const WtEditBounds zone_bounds{ zone.minimum, zone.maximum };
		for (EditLodRetentionZone &existing : edit_lod_retention_zones_) {
			const WtEditBounds existing_bounds{
				existing.minimum,
				existing.maximum
			};
			if (bounds_distance_squared(existing_bounds, zone_bounds) >
				merge_distance_squared) {
				continue;
			}
			const WtGridPoint merged_minimum{
				std::min(existing.minimum.x, zone.minimum.x),
				std::min(existing.minimum.y, zone.minimum.y),
				std::min(existing.minimum.z, zone.minimum.z),
			};
			const WtGridPoint merged_maximum{
				std::max(existing.maximum.x, zone.maximum.x),
				std::max(existing.maximum.y, zone.maximum.y),
				std::max(existing.maximum.z, zone.maximum.z),
			};
			if (edit_lod_retention_unbounded_refinement_radius(
					merged_minimum,
					merged_maximum
				) > kWtEditLodRetentionMaximumRefinementRadiusChunks) {
				continue;
			}
			existing.minimum = merged_minimum;
			existing.maximum = merged_maximum;
			existing.x = bounds_center_axis(
				existing.minimum.x,
				existing.maximum.x
			);
			existing.y = bounds_center_axis(
				existing.minimum.y,
				existing.maximum.y
			);
			existing.z = bounds_center_axis(
				existing.minimum.z,
				existing.maximum.z
			);
			existing.refinement_radius_chunks =
				edit_lod_retention_refinement_radius(
					existing.minimum,
					existing.maximum
				);
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
	std::vector<WtLodPlannerViewer> &planning_viewers,
	std::uint32_t maximum_refinement_radius_chunks,
	std::size_t maximum_retention_viewers
) const {
	if (real_viewers.empty() || edit_lod_retention_zones_.empty() ||
			maximum_refinement_radius_chunks == 0 ||
			maximum_retention_viewers == 0) {
		return 0;
	}
	std::uint8_t maximum_lod = 0;
	for (const WtLodPlannerViewer &viewer : real_viewers) {
		maximum_lod = std::max(maximum_lod, viewer.maximum_lod);
	}
	std::size_t appended = 0;
	const auto is_recent_zone = [this](const EditLodRetentionZone &zone) {
		if (edit_lod_retention_zones_.size() <=
			kWtEditLodRetentionAlwaysActiveRecentZones) {
			return true;
		}
		std::size_t newer = 0;
		for (const EditLodRetentionZone &candidate :
				edit_lod_retention_zones_) {
			if (candidate.revision > zone.revision) {
				++newer;
				if (newer >= kWtEditLodRetentionAlwaysActiveRecentZones) {
					return false;
				}
			}
		}
		return true;
	};
	std::vector<const EditLodRetentionZone *> visible_zones;
	visible_zones.reserve(edit_lod_retention_zones_.size());
	for (const EditLodRetentionZone &zone : edit_lod_retention_zones_) {
		bool visible_to_real_viewer = is_recent_zone(zone);
		for (const WtLodPlannerViewer &viewer : real_viewers) {
			const double root_extent =
				static_cast<double>(wt_chunk_extent(viewer.maximum_lod));
			const double active_distance =
				(static_cast<double>(viewer.radius_chunks) +
					kWtEditLodRetentionVisibilitySlackRoots) * root_extent;
			if (point_interval_distance(
					viewer.snapshot.x,
					zone.minimum.x,
					zone.maximum.x
				) <= active_distance &&
				point_interval_distance(
					viewer.snapshot.z,
					zone.minimum.z,
					zone.maximum.z
				) <= active_distance) {
				visible_to_real_viewer = true;
				break;
			}
		}
		if (!visible_to_real_viewer) {
			continue;
		}
		visible_zones.push_back(&zone);
	}
	std::sort(
		visible_zones.begin(),
		visible_zones.end(),
		[](const EditLodRetentionZone *left,
			const EditLodRetentionZone *right) {
			return left->revision > right->revision;
		}
	);
	const std::size_t append_limit = std::min(
		visible_zones.size(),
		maximum_retention_viewers
	);
	for (std::size_t index = 0; index < append_limit; ++index) {
		const EditLodRetentionZone &zone = *visible_zones[index];
		planning_viewers.push_back({
			{
				zone.viewer_id,
				zone.x,
				zone.y,
				zone.z,
				zone.revision,
			},
			kWtEditLodRetentionRootRadiusChunks,
			maximum_lod,
			std::min(
				zone.refinement_radius_chunks,
				maximum_refinement_radius_chunks
			),
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
	std::vector<CollisionViewer> candidate_collision_viewers =
		collision_viewers_;
	const bool collision_event =
		event.kind == ViewerEventKind::UpdateCollision ||
		event.kind == ViewerEventKind::RemoveCollision;
	if (!collision_event) {
		const auto viewer = std::lower_bound(
			candidate_viewers.begin(),
			candidate_viewers.end(),
			event.snapshot.id,
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
	} else {
		const auto viewer = std::lower_bound(
			candidate_collision_viewers.begin(),
			candidate_collision_viewers.end(),
			event.snapshot.id,
			[](const CollisionViewer &item, std::uint64_t id) {
				return item.snapshot.id < id;
			}
		);
		if (event.kind == ViewerEventKind::UpdateCollision) {
			if (viewer != candidate_collision_viewers.end() &&
				viewer->snapshot.id == event.snapshot.id) {
				if (event.snapshot.revision <= viewer->snapshot.revision) {
					std::lock_guard<std::mutex> lock(metrics_mutex_);
					++metrics_.rejected_events;
					return true;
				}
				*viewer = { event.snapshot, event.radius_chunks };
			} else if (candidate_collision_viewers.size() >=
				config_.viewer_capacity) {
				std::lock_guard<std::mutex> lock(metrics_mutex_);
				++metrics_.rejected_events;
				return true;
			} else {
				candidate_collision_viewers.insert(
					viewer,
					{ event.snapshot, event.radius_chunks }
				);
			}
		} else {
			if (viewer == candidate_collision_viewers.end() ||
				viewer->snapshot.id != event.snapshot.id ||
				event.snapshot.revision <= viewer->snapshot.revision) {
				std::lock_guard<std::mutex> lock(metrics_mutex_);
				++metrics_.rejected_events;
				return true;
			}
			candidate_collision_viewers.erase(viewer);
		}
	}

	const WtCollisionPolicy collision_policy {
		kWtDefaultCollisionThinRatioSquared,
		config_.collision_activation_distance,
		config_.collision_deactivation_distance,
	};
	bool edit_retention_fallback = false;
	std::vector<WtLodPlannerViewer> planning_viewers;
	std::size_t edit_retention_viewers = 0;
	WtBalancedLodPlan candidate_plan;
	WtBalancedLodPlannerStatus plan_status = WtBalancedLodPlannerStatus::Ok;
	if (collision_event) {
		// Collision viewers are an independent working-set overlay. Reusing the
		// current visual plan avoids retraversing and reprioritizing the entire
		// visual LOD tree for a movement that cannot change visual topology.
		planning_viewers = candidate_viewers;
		candidate_plan = current_plan_;
	} else {
		const std::size_t retention_viewer_capacity =
			kWtEditLodRetentionCapacity;
		const auto try_plan_with_retention =
			[&](
				std::uint32_t maximum_refinement_radius_chunks,
				std::size_t maximum_retention_viewers
			) {
				planning_viewers = candidate_viewers;
				candidate_plan.clear();
				edit_retention_viewers = append_edit_lod_retention_viewers(
					candidate_viewers,
					planning_viewers,
					maximum_refinement_radius_chunks,
					maximum_retention_viewers
				);
				return lod_planner_->plan(
					planning_viewers,
					desired_->get_desired_chunks(),
					collision_policy,
					candidate_plan
				);
			};
		plan_status = try_plan_with_retention(
			kWtEditLodRetentionMaximumRefinementRadiusChunks,
			retention_viewer_capacity
		);
		if (plan_status != WtBalancedLodPlannerStatus::Ok &&
				edit_retention_viewers != 0) {
			edit_retention_fallback = true;
			const std::size_t retry_retention_viewers = edit_retention_viewers;
			bool accepted_degraded_retention = false;
			for (std::uint32_t radius =
					kWtEditLodRetentionMaximumRefinementRadiusChunks;
					radius >= kWtEditLodRetentionMinimumRefinementRadiusChunks;
					--radius) {
				std::size_t viewer_limit = retry_retention_viewers;
				if (radius == kWtEditLodRetentionMaximumRefinementRadiusChunks) {
					if (viewer_limit == 0) {
						break;
					}
					--viewer_limit;
				}
				while (viewer_limit > 0) {
					plan_status = try_plan_with_retention(radius, viewer_limit);
					if (plan_status == WtBalancedLodPlannerStatus::Ok &&
							edit_retention_viewers != 0) {
						accepted_degraded_retention = true;
						break;
					}
					--viewer_limit;
				}
				if (accepted_degraded_retention ||
						radius ==
							kWtEditLodRetentionMinimumRefinementRadiusChunks) {
					break;
				}
			}
			if (!accepted_degraded_retention) {
				edit_retention_viewers = 0;
				planning_viewers = candidate_viewers;
				candidate_plan.clear();
				plan_status = lod_planner_->plan(
					planning_viewers,
					desired_->get_desired_chunks(),
					collision_policy,
					candidate_plan
				);
			}
		}
	}
	if (plan_status != WtBalancedLodPlannerStatus::Ok ||
			plan_revision_ == std::numeric_limits<std::uint64_t>::max()) {
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.rejected_events;
		return true;
	}

	std::vector<WtViewerChunkDemand> combined_demands =
		candidate_plan.demands;
	for (const CollisionViewer &collision_viewer :
		candidate_collision_viewers) {
		std::int32_t center_x = 0;
		std::int32_t center_y = 0;
		std::int32_t center_z = 0;
		if (!chunk_coordinate(collision_viewer.snapshot.x, center_x) ||
			!chunk_coordinate(collision_viewer.snapshot.y, center_y) ||
			!chunk_coordinate(collision_viewer.snapshot.z, center_z)) {
			std::lock_guard<std::mutex> lock(metrics_mutex_);
			++metrics_.rejected_events;
			return true;
		}
		const std::int64_t radius = collision_viewer.radius_chunks;
		for (std::int64_t z = -radius; z <= radius; ++z) {
			for (std::int64_t y = -radius; y <= radius; ++y) {
				for (std::int64_t x = -radius; x <= radius; ++x) {
					const std::int64_t distance_squared =
						x * x + y * y + z * z;
					if (distance_squared > radius * radius) {
						continue;
					}
					const std::int64_t key_x =
						static_cast<std::int64_t>(center_x) + x;
					const std::int64_t key_y =
						static_cast<std::int64_t>(center_y) + y;
					const std::int64_t key_z =
						static_cast<std::int64_t>(center_z) + z;
					if (key_x < std::numeric_limits<std::int32_t>::min() ||
						key_x > std::numeric_limits<std::int32_t>::max() ||
						key_y < std::numeric_limits<std::int32_t>::min() ||
						key_y > std::numeric_limits<std::int32_t>::max() ||
						key_z < std::numeric_limits<std::int32_t>::min() ||
						key_z > std::numeric_limits<std::int32_t>::max()) {
						continue;
					}
					const WtChunkKey key {
						static_cast<std::int32_t>(key_x),
						static_cast<std::int32_t>(key_y),
						static_cast<std::int32_t>(key_z),
						0,
					};
					if (!std::binary_search(
							page_keys_.begin(), page_keys_.end(), key
						)) {
						continue;
					}
					combined_demands.push_back({
						key,
						kWtCollisionInvokerPriorityMaximum -
							static_cast<std::int32_t>(distance_squared),
						true,
						false,
					});
				}
			}
		}
	}
	std::sort(
		combined_demands.begin(),
		combined_demands.end(),
		[](const WtViewerChunkDemand &left,
			const WtViewerChunkDemand &right) {
			return left.key < right.key;
		}
	);
	std::vector<WtViewerChunkDemand> merged_demands;
	merged_demands.reserve(combined_demands.size());
	for (const WtViewerChunkDemand &demand : combined_demands) {
		if (!merged_demands.empty() &&
			merged_demands.back().key == demand.key) {
			WtViewerChunkDemand &merged = merged_demands.back();
			merged.priority = std::max(merged.priority, demand.priority);
			merged.collision_required =
				merged.collision_required || demand.collision_required;
			merged.visual_required =
				merged.visual_required || demand.visual_required;
		} else {
			merged_demands.push_back(demand);
		}
	}
	if (merged_demands.size() > config_.active_chunk_capacity) {
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
			plan_snapshot, merged_demands, delta
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
	// The front-end stages every outgoing render until replacement readiness is
	// satisfied, including removals that arrive in a later viewer update than
	// their additions. An outgoing visual-only chunk must therefore gain
	// collision for the same interval. Capture its generation and collision
	// payload before apply_delta erases the worker-side record and cache entries.
	std::vector<WtReadOnlyPublication> outgoing_collision_publications;
	if (!delta.removed.empty()) {
		outgoing_collision_publications.reserve(delta.removed.size() * 2U);
		const WtCollisionPolicy outgoing_collision_policy {
			kWtDefaultCollisionThinRatioSquared,
			config_.collision_activation_distance,
			config_.collision_deactivation_distance,
		};
		for (const WtChunkKey &key : delta.removed) {
			const WtDesiredChunk *outgoing = desired_->find_desired(key);
			if (outgoing == nullptr || outgoing->collision_required) continue;
			const WtChunkRecord *record = scheduler_->find_record(key);
			if (record == nullptr) {
				set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaStateMismatch);
				return true;
			}
			std::shared_ptr<const WtCollisionPayload> collision;
			const WtChunkResourceCacheStatus collision_status =
				resource_cache_->find_or_rebuild_collision(
					key,
					record->generation,
					outgoing_collision_policy,
					collision,
					!collision_viewers_.empty()
				);
			if (collision_status != WtChunkResourceCacheStatus::Ok &&
				collision_status != WtChunkResourceCacheStatus::NotFound) {
				set_failure(WtReadOnlyRuntimeStatus::PipelineFailure);
				return true;
			}
			if (!collision) continue;

			WtReadOnlyPublication requirement;
			requirement.kind =
				WtReadOnlyPublicationKind::SetCollisionRequired;
			requirement.key = key;
			requirement.generation = record->generation;
			requirement.collision_required = true;
			outgoing_collision_publications.push_back(
				std::move(requirement)
			);

			WtReadOnlyPublication payload;
			payload.kind = WtReadOnlyPublicationKind::CollisionPayload;
			payload.key = key;
			payload.generation = record->generation;
			payload.collision_required = true;
			payload.collision = std::move(collision);
			outgoing_collision_publications.push_back(std::move(payload));
		}
	}
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
	for (WtReadOnlyPublication &publication :
		outgoing_collision_publications) {
		if (!push_publication(std::move(publication))) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			return true;
		}
	}
	if (!publish_delta(delta)) {
		if (!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
		return true;
	}
	const std::size_t planned_demand_count = merged_demands.size();
	*desired_ = std::move(candidate_desired);
	planner_viewers_ = std::move(candidate_viewers);
	collision_viewers_ = std::move(candidate_collision_viewers);
	current_plan_ = std::move(candidate_plan);
	queue_transition_remeshes(transition_remeshes);
	plan_revision_ = plan_snapshot.revision;
	std::vector<WtChunkKey> active_keys;
	active_keys.reserve(desired_->get_desired_chunks().size());
	for (const WtDesiredChunk &item : desired_->get_desired_chunks()) {
		active_keys.push_back(item.key);
	}
	if (edit_spatial_index_->rebuild(active_keys) != WtEditSpatialStatus::Ok) {
		set_failure(WtReadOnlyRuntimeStatus::EditFailure);
		return true;
	}
	for (const WtDesiredChunk &item : delta.updated) {
		if (!item.collision_required) continue;
		const WtChunkRecord *record = scheduler_->find_record(item.key);
		if (record == nullptr) continue;
		std::shared_ptr<const WtCollisionPayload> collision;
		const WtCollisionPolicy collision_policy {
			kWtDefaultCollisionThinRatioSquared,
			config_.collision_activation_distance,
			config_.collision_deactivation_distance,
		};
		const WtChunkResourceCacheStatus collision_status =
			resource_cache_->find_or_rebuild_collision(
				item.key,
				record->generation,
				collision_policy,
				collision,
				!collision_viewers_.empty()
			);
		if (collision_status != WtChunkResourceCacheStatus::Ok &&
			collision_status != WtChunkResourceCacheStatus::NotFound) {
			set_failure(WtReadOnlyRuntimeStatus::PipelineFailure);
			return true;
		}
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
		} else if (event.kind == ViewerEventKind::Remove) {
			++metrics_.viewer_removals;
		} else if (event.kind == ViewerEventKind::UpdateCollision) {
			++metrics_.collision_viewer_updates;
			metrics_.planned_demands += planned_demand_count;
		} else {
			++metrics_.collision_viewer_removals;
		}
		if (!collision_event) {
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

void WtReadOnlyWorldRuntime::queue_readiness_repair_candidate(
	const WtChunkKey &key
) {
	const auto position = std::lower_bound(
		readiness_repair_candidate_keys_.begin(),
		readiness_repair_candidate_keys_.end(),
		key
	);
	if (position == readiness_repair_candidate_keys_.end() ||
		*position != key) {
		readiness_repair_candidate_keys_.insert(position, key);
	}
}

bool WtReadOnlyWorldRuntime::publish_delta(
	const WtDesiredSetDelta &delta
) {
	// Publish additions before removals. The Godot/front-end application keeps
	// old visible chunks alive while replacements are staged, but it can only do
	// that correctly for chunks it already knows are expected. If removals are
	// published first during a large viewer movement, the front-end can retire
	// old chunks before it has received all new chunk expectations, producing
	// visible rectangular skybox holes while the scheduler is still working.
	bool contains_replacement = !delta.removed.empty();
	for (const WtDesiredChunk &item : delta.updated) {
		const WtDesiredChunk *previous = desired_->find_desired(item.key);
		contains_replacement = contains_replacement ||
			(previous != nullptr && previous->visual_required &&
				!item.visual_required);
	}
	for (const WtDesiredChunk &item : delta.added) {
		const WtChunkRecord *record = scheduler_->find_record(item.key);
		if (record == nullptr) return false;
		const bool addition_staged_replacement = contains_replacement;
		WtReadOnlyPublication publication;
		publication.kind = WtReadOnlyPublicationKind::ExpectChunk;
		publication.key = item.key;
		publication.generation = record->generation;
		publication.collision_required = item.collision_required;
		publication.visual_required = item.visual_required;
		publication.staged_replacement = addition_staged_replacement;
		if (!push_publication(std::move(publication))) return false;
		if (addition_staged_replacement) {
			const WtApplicationStatus application_status =
				application_->expect_chunk(
					item.key,
					record->generation,
					item.collision_required,
					item.visual_required,
					true,
					item.collision_required
				);
			if (application_status != WtApplicationStatus::Ok &&
				application_status != WtApplicationStatus::AlreadyCurrent) {
				return false;
			}
			queue_readiness_repair_candidate(item.key);
		}
		const auto render = item.visual_required ?
			resource_cache_->find_render(item.key, record->generation) :
			std::shared_ptr<const WtRenderPayload>{};
		if (render) {
			WtReadOnlyPublication render_publication;
			render_publication.kind = WtReadOnlyPublicationKind::RenderPayload;
			render_publication.key = render->key;
			render_publication.generation = render->generation;
			render_publication.render = render;
			render_publication.staged_replacement =
				addition_staged_replacement;
			if (!push_publication(std::move(render_publication))) return false;
		}
		if (item.collision_required) {
			const auto collision = resource_cache_->find_collision(
				item.key,
				record->generation
			);
			if (collision) {
				WtReadOnlyPublication collision_publication;
				collision_publication.kind =
					WtReadOnlyPublicationKind::CollisionPayload;
				collision_publication.key = collision->key;
				collision_publication.generation = collision->generation;
				collision_publication.collision_required = true;
				collision_publication.collision = collision;
				if (!push_publication(std::move(collision_publication))) return false;
			}
		}
	}
	for (const WtDesiredChunk &item : delta.updated) {
		const WtDesiredChunk *previous = desired_->find_desired(item.key);
		if (previous == nullptr) return false;
		// Numeric job priority changes on almost every viewer movement and is
		// consumed by the worker-side scheduler. Publishing those changes as
		// collision state floods the bounded front-end queue with obsolete
		// SetCollisionRequired messages, delaying the first real activation.
		const WtChunkRecord *record = scheduler_->find_record(item.key);
		if (record == nullptr) return false;
		if (previous->collision_required != item.collision_required &&
			item.collision_required && !push_publication({
				WtReadOnlyPublicationKind::SetCollisionRequired,
				item.key,
				record->generation,
				true,
				{},
				{},
			})) return false;
		if (previous->visual_required != item.visual_required) {
			if (item.visual_required) {
				WtReadOnlyPublication expectation;
				expectation.kind = WtReadOnlyPublicationKind::ExpectChunk;
				expectation.key = item.key;
				expectation.generation = record->generation;
				expectation.collision_required = item.collision_required;
				expectation.visual_required = true;
				expectation.staged_replacement = true;
				const WtApplicationStatus application_status =
					application_->expect_chunk(
						item.key,
						record->generation,
						item.collision_required,
						true,
						true,
						item.collision_required
					);
				if (application_status != WtApplicationStatus::Ok &&
					application_status != WtApplicationStatus::AlreadyCurrent) {
					return false;
				}
				if (!push_publication(std::move(expectation))) return false;
				queue_readiness_repair_candidate(item.key);
			}
			WtReadOnlyPublication visual;
			visual.kind = WtReadOnlyPublicationKind::SetVisualRequired;
			visual.key = item.key;
			visual.generation = record->generation;
			visual.visual_required = item.visual_required;
			visual.staged_replacement = !item.visual_required;
			if (!push_publication(std::move(visual))) return false;
		}
		if (previous->collision_required != item.collision_required &&
			!item.collision_required && !push_publication({
				WtReadOnlyPublicationKind::SetCollisionRequired,
				item.key,
				record->generation,
				false,
				{},
				{},
			})) return false;
	}
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
				desired->collision_required,
				desired->visual_required,
				true,
				desired->collision_required
			);
		if (application_status != WtApplicationStatus::Ok &&
			application_status != WtApplicationStatus::AlreadyCurrent) {
			set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
			return true;
		}
		WtReadOnlyPublication expectation;
		expectation.kind = WtReadOnlyPublicationKind::ExpectChunk;
		expectation.key = item.key;
		expectation.generation = record->generation;
		expectation.collision_required = desired->collision_required;
		expectation.visual_required = desired->visual_required;
		expectation.staged_replacement = true;
		expectation.preserve_collision_ready = desired->collision_required;
		if (!push_publication(std::move(expectation))) {
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
			status = page_runtime_->begin_sample_job(
				job,
				entry != nullptr ? entry->transition_mask : 0,
				storage_,
				*page_cache_,
				*scheduler_
			);
			std::lock_guard<std::mutex> lock(metrics_mutex_);
			++metrics_.sample_jobs;
		} else {
			const WtChunkApplicationRecord *application_record =
				application_->find_record(job.key);
			if (application_record == nullptr ||
				application_record->generation != job.generation) {
				continue;
			}
			status = page_runtime_->execute_mesh_job(
				job,
				*mesher_,
				*meshing_scratch_,
				*scheduler_,
				edit_journal_store_ != nullptr ?
					&edit_journal_store_->journal() : nullptr,
				initial_world_revision_,
				&storage_,
				[this](const WtTerrainMeshCompletion &completion) {
					return process_terrain_mesh_completion(completion);
				},
				application_record->visual_required
			);
			std::lock_guard<std::mutex> lock(metrics_mutex_);
			++metrics_.mesh_jobs;
		}
		if (status ==
				WtPageMeshingRuntimeStatus::TerrainMeshReadyCallbackFailure) {
			break;
		}
		if (status != WtPageMeshingRuntimeStatus::Ok &&
			status != WtPageMeshingRuntimeStatus::SchedulerBackpressure &&
			status != WtPageMeshingRuntimeStatus::StorageRequestFailure &&
			status != WtPageMeshingRuntimeStatus::CacheFailure &&
			status != WtPageMeshingRuntimeStatus::MeshingFailure &&
			status != WtPageMeshingRuntimeStatus::SurfaceShiftFailure &&
			status != WtPageMeshingRuntimeStatus::NotReady) {
			set_failure(WtReadOnlyRuntimeStatus::PipelineFailure);
			break;
		}
		if (has_pending_edit_operation()) {
			break;
		}
	}
	return progressed;
}

bool WtReadOnlyWorldRuntime::process_terrain_mesh_completion(
	const WtTerrainMeshCompletion &completion
) {
	const WtChunkRecord *record = scheduler_->find_record(completion.key);
	if (record == nullptr || record->generation != completion.generation ||
		!completion.mesh) {
		return true;
	}
	auto terrain_render = std::make_shared<WtRenderPayload>();
	auto collision = std::make_shared<WtCollisionPayload>();
	const WtCollisionPolicy collision_policy {
		kWtDefaultCollisionThinRatioSquared,
		config_.collision_activation_distance,
		config_.collision_deactivation_distance,
	};
	const WtChunkApplicationRecord *application_record =
		application_->find_record(completion.key);
	if (application_record == nullptr ||
		application_record->generation != completion.generation) {
		return true;
	}
	const bool regular_collision =
		!application_record->visual_required || !collision_viewers_.empty();
	if (resource_cache_->insert_mesh(
			completion.mesh,
			completion.generation,
			record->generation
		) != WtChunkResourceCacheStatus::Ok ||
		(regular_collision ?
			wt_build_regular_collision_payload(
				*completion.mesh,
				completion.generation,
				collision_policy,
				*collision
			) :
			(wt_build_render_payload(
				*completion.mesh,
				completion.generation,
				*terrain_render
			) == WtRenderBuildStatus::Ok ?
					wt_build_collision_payload(
						*terrain_render,
						collision_policy,
						*collision
					) : WtCollisionBuildStatus::InvalidMesh)) !=
				WtCollisionBuildStatus::Ok ||
		resource_cache_->insert_collision(collision, record->generation) !=
			WtChunkResourceCacheStatus::Ok) {
		set_failure(WtReadOnlyRuntimeStatus::PipelineFailure);
		return false;
	}
	if (application_record->collision_required &&
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
		return false;
	}
	return true;
}

bool WtReadOnlyWorldRuntime::process_mesh_completions() {
	bool progressed = false;
	WtPageMeshCompletion completion;
	while (page_runtime_->pop_mesh_completion(completion)) {
		progressed = true;
		const WtChunkRecord *record = scheduler_->find_record(completion.key);
		if (record == nullptr || record->generation != completion.generation ||
			!completion.mesh || !completion.water_mesh) {
			continue;
		}
		const WtChunkApplicationRecord *application_record =
			application_->find_record(completion.key);
		if (application_record == nullptr ||
			application_record->generation != completion.generation ||
			!application_record->visual_required) {
			continue;
		}
		auto render = std::make_shared<WtRenderPayload>();
		if (resource_cache_->insert_mesh(
				completion.mesh,
				completion.generation,
				record->generation
			) != WtChunkResourceCacheStatus::Ok ||
			wt_build_render_payload(
				*completion.mesh,
				*completion.water_mesh,
				completion.generation,
				*render
			) != WtRenderBuildStatus::Ok ||
			resource_cache_->insert_render(render, record->generation) !=
				WtChunkResourceCacheStatus::Ok) {
			set_failure(WtReadOnlyRuntimeStatus::PipelineFailure);
			break;
		}
		WtReadOnlyPublication publication;
		publication.kind = WtReadOnlyPublicationKind::RenderPayload;
		publication.key = render->key;
		publication.generation = render->generation;
		publication.render = render;
		publication.staged_replacement = application_record->staged_replacement;
		if (!push_publication(std::move(publication))) {
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

bool WtReadOnlyWorldRuntime::process_visual_readiness_repairs() {
	if (!desired_) return false;
	if (has_publication_backlog() || application_->queued_render_count() != 0) {
		return false;
	}
	const std::uint64_t application_progress_sequence =
		application_progress_sequence_.load(std::memory_order_relaxed);
	if (readiness_repair_application_sequence_ !=
		application_progress_sequence) {
		readiness_repair_application_sequence_ = application_progress_sequence;
		readiness_render_repairs_this_sequence_ = 0;
	}
	constexpr std::size_t kMaxRenderRepairsPerApplicationProgress = 16;
	const WtSchedulerMetrics scheduler_metrics = scheduler_->get_metrics();
	if (scheduler_->queued_job_count() != 0 ||
		scheduler_->queued_completion_count() != 0 ||
		scheduler_metrics.sampling_records != 0 ||
		scheduler_metrics.meshing_records != 0) {
		return false;
	}
	bool progressed = false;
	std::size_t repairs = 0;
	readiness_repair_attempts_.erase(
		std::remove_if(
			readiness_repair_attempts_.begin(),
			readiness_repair_attempts_.end(),
			[this](const ReadinessRepairAttempt &attempt) {
				const WtChunkApplicationRecord *record =
					application_->find_record(attempt.key);
				return record == nullptr ||
					record->generation != attempt.generation ||
					!record->staged_replacement;
			}
		),
		readiness_repair_attempts_.end()
	);
	readiness_repair_remesh_attempts_.erase(
		std::remove_if(
			readiness_repair_remesh_attempts_.begin(),
			readiness_repair_remesh_attempts_.end(),
			[this](const ReadinessRepairRemeshAttempt &attempt) {
				const WtChunkApplicationRecord *record =
					application_->find_record(attempt.key);
				if (record == nullptr || !record->staged_replacement ||
					record->generation != attempt.generation) {
					return true;
				}
				const WtChunkRecord *scheduler_record =
					scheduler_->find_record(attempt.key);
				return scheduler_record != nullptr &&
					scheduler_record->lifecycle == WtChunkLifecycle::Ready &&
					!resource_cache_->find_render(
						attempt.key,
						attempt.generation
					);
			}
		),
		readiness_repair_remesh_attempts_.end()
	);
	const auto find_attempt = [this](
		const WtChunkKey &key,
		WtGenerationToken generation
	) -> ReadinessRepairAttempt * {
		for (ReadinessRepairAttempt &attempt :
				readiness_repair_attempts_) {
			if (attempt.key == key && attempt.generation == generation) {
				return &attempt;
			}
		}
		return nullptr;
	};
	const auto ensure_attempt = [&](
		const WtChunkKey &key,
		WtGenerationToken generation
	) -> ReadinessRepairAttempt & {
		if (ReadinessRepairAttempt *attempt = find_attempt(key, generation)) {
			return *attempt;
		}
		readiness_repair_attempts_.push_back({ key, generation });
		return readiness_repair_attempts_.back();
	};
	const auto remesh_already_requested = [this](
		const WtChunkKey &key,
		WtGenerationToken generation
	) {
		return std::find_if(
			readiness_repair_remesh_attempts_.begin(),
			readiness_repair_remesh_attempts_.end(),
			[&](const ReadinessRepairRemeshAttempt &attempt) {
				return attempt.key == key && attempt.generation == generation;
			}
		) != readiness_repair_remesh_attempts_.end();
	};
	enum class RepairResult {
		Skipped,
		Waiting,
		Repaired,
		CapacityBlocked,
		Failed,
	};
	const auto process_item = [&](const WtDesiredChunk &item) -> RepairResult {
		if (repairs >= 64U || scheduler_->available_job_capacity() == 0) {
			return RepairResult::CapacityBlocked;
		}
		if (!item.visual_required) return RepairResult::Skipped;
		const WtChunkRecord *record = scheduler_->find_record(item.key);
		if (record == nullptr || record->lifecycle != WtChunkLifecycle::Ready) {
			return RepairResult::Waiting;
		}
		const WtChunkApplicationRecord *application_record =
			application_->find_record(item.key);
		const bool staged_replacement =
			application_record != nullptr &&
			application_record->generation == record->generation &&
			application_record->staged_replacement;
		if (staged_replacement) {
			const auto render =
				resource_cache_->find_render(item.key, record->generation);
			if (!render) {
				if (remesh_already_requested(item.key, record->generation)) {
					return RepairResult::Skipped;
				}
				const WtSchedulerStatus scheduler_status =
					scheduler_->request_chunk_version(
						item.key,
						storage_.source_revision(),
						world_revision_.load(),
						item.priority,
						true
					);
				if (scheduler_status == WtSchedulerStatus::JobQueueFull) {
					return RepairResult::CapacityBlocked;
				}
				if (scheduler_status != WtSchedulerStatus::Ok) {
					set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
					return RepairResult::Failed;
				}
				record = scheduler_->find_record(item.key);
				const WtApplicationStatus application_status =
					record == nullptr ? WtApplicationStatus::NotFound :
					application_->expect_chunk(
						item.key,
						record->generation,
						item.collision_required,
						item.visual_required,
						true,
						item.collision_required
					);
				if (application_status != WtApplicationStatus::Ok &&
					application_status != WtApplicationStatus::AlreadyCurrent) {
					set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
					return RepairResult::Failed;
				}
				WtReadOnlyPublication publication;
				publication.kind = WtReadOnlyPublicationKind::ExpectChunk;
				publication.key = item.key;
				publication.generation = record->generation;
				publication.collision_required = item.collision_required;
				publication.visual_required = item.visual_required;
				publication.staged_replacement = true;
				publication.preserve_collision_ready = item.collision_required;
				if (!push_publication(std::move(publication))) {
					if (!stop_requested_.load()) {
						set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
					}
					return RepairResult::Failed;
				}
				readiness_repair_remesh_attempts_.push_back({
					item.key,
					record->generation,
				});
				++repairs;
				progressed = true;
				return RepairResult::Repaired;
			}
			ReadinessRepairAttempt &attempt =
				ensure_attempt(item.key, record->generation);
			if (attempt.render_republished &&
				attempt.render_republish_application_sequence ==
					application_progress_sequence) {
				return RepairResult::Skipped;
			}
			if (readiness_render_repairs_this_sequence_ >=
				kMaxRenderRepairsPerApplicationProgress) {
				return RepairResult::CapacityBlocked;
			}
			WtReadOnlyPublication publication;
			publication.kind = WtReadOnlyPublicationKind::RenderPayload;
			publication.key = render->key;
			publication.generation = render->generation;
			publication.render = render;
			publication.staged_replacement = true;
			if (!push_publication(std::move(publication))) {
				if (!stop_requested_.load()) {
					set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
				}
				return RepairResult::Failed;
			}
			attempt.render_republished = true;
			attempt.render_republish_application_sequence =
				application_progress_sequence;
			++readiness_render_repairs_this_sequence_;
			++repairs;
			progressed = true;
			return RepairResult::Repaired;
		}
		if (resource_cache_->find_render(item.key, record->generation)) {
			return RepairResult::Skipped;
		}
		const WtSchedulerStatus scheduler_status =
			scheduler_->request_chunk_version(
				item.key,
				storage_.source_revision(),
				world_revision_.load(),
				item.priority,
				true
			);
		if (scheduler_status == WtSchedulerStatus::JobQueueFull) {
			return RepairResult::CapacityBlocked;
		}
		if (scheduler_status != WtSchedulerStatus::Ok) {
			set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
			return RepairResult::Failed;
		}
		record = scheduler_->find_record(item.key);
		const WtApplicationStatus application_status =
			record == nullptr ? WtApplicationStatus::NotFound :
			application_->expect_chunk(
				item.key,
				record->generation,
				item.collision_required,
				item.visual_required,
				false,
				false
			);
		if (application_status != WtApplicationStatus::Ok &&
			application_status != WtApplicationStatus::AlreadyCurrent) {
			set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
			return RepairResult::Failed;
		}
		WtReadOnlyPublication publication;
		publication.kind = WtReadOnlyPublicationKind::ExpectChunk;
		publication.key = item.key;
		publication.generation = record->generation;
		publication.collision_required = item.collision_required;
		publication.visual_required = item.visual_required;
		publication.staged_replacement = false;
		publication.preserve_collision_ready = false;
		if (!push_publication(std::move(publication))) {
			if (!stop_requested_.load()) {
				set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
			}
			return RepairResult::Failed;
		}
		++repairs;
		progressed = true;
		return RepairResult::Repaired;
	};
	for (std::size_t index = 0;
			index < readiness_repair_candidate_keys_.size();) {
		if (repairs >= 64U || scheduler_->available_job_capacity() == 0) {
			break;
		}
		const WtDesiredChunk *desired =
			desired_->find_desired(readiness_repair_candidate_keys_[index]);
		if (desired == nullptr || !desired->visual_required) {
			readiness_repair_candidate_keys_.erase(
				readiness_repair_candidate_keys_.begin() + index
			);
			continue;
		}
		const RepairResult result = process_item(*desired);
		if (result == RepairResult::Failed) {
			return progressed;
		}
		if (result == RepairResult::CapacityBlocked) {
			break;
		}
		if (result == RepairResult::Waiting) {
			++index;
			continue;
		}
		readiness_repair_candidate_keys_.erase(
			readiness_repair_candidate_keys_.begin() + index
		);
	}
	for (const WtDesiredChunk &item : desired_->get_desired_chunks()) {
		if (repairs >= 64U || scheduler_->available_job_capacity() == 0) {
			break;
		}
		const RepairResult result = process_item(item);
		if (result == RepairResult::Failed) {
			return progressed;
		}
		if (result == RepairResult::CapacityBlocked) {
			break;
		}
	}
	return progressed;
}

} // namespace world_transvoxel
