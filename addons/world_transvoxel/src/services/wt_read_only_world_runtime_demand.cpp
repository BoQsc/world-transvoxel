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
	const bool trace_enabled = causal_trace_.enabled();
	if (trace_enabled) {
		causal_trace_.record(
			WtCausalTraceEventKind::ViewerPlanStarted,
			WtCausalTraceThreadRole::Runtime,
			nullptr,
			{},
			event.snapshot.revision,
			static_cast<std::uint64_t>(event.kind)
		);
	}
	const std::uint64_t planning_started_ns = trace_enabled ?
		wt_causal_trace_now_ns() : 0;
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
					candidate_plan,
					config_.visual_viewer_collision_enabled
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
					candidate_plan,
					config_.visual_viewer_collision_enabled
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
					if (!storage_.has_page(key)) {
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

	const std::vector<WtViewerChunkDemand> candidate_base_demands =
		merged_demands;
	const WtForegroundPriorityOverlayResult foreground_overlay =
		foreground_priority_leases_.apply(
			candidate_base_demands,
			merged_demands
		);
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

	std::vector<WtLodMapEntry> transition_mask_updates;
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
		transition_mask_updates.push_back(*next);
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
					true
				);
			if (collision_status != WtChunkResourceCacheStatus::Ok &&
				collision_status != WtChunkResourceCacheStatus::NotFound) {
				set_failure(
					WtReadOnlyRuntimeStatus::PipelineCollisionRebuildFailure
				);
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
		set_failure(delta_failure_status(delta_status));
		return true;
	}
	if (trace_enabled) {
		for (const WtDesiredChunk &item : delta.added) {
			const WtChunkRecord *record = scheduler_->find_record(item.key);
			if (record != nullptr) {
				causal_trace_.record(
					WtCausalTraceEventKind::ChunkDemandAccepted,
					WtCausalTraceThreadRole::Runtime,
					&item.key,
					record->generation,
					event.snapshot.revision,
					static_cast<std::uint64_t>(item.priority)
				);
			}
		}
	}
	WtReadOnlyPublication plan_started;
	plan_started.kind = WtReadOnlyPublicationKind::ViewerPlanStarted;
	plan_started.world_revision = plan_snapshot.revision;
	if (!push_publication(std::move(plan_started))) {
		if (!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
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
	base_demands_ = candidate_base_demands;
	planner_viewers_ = std::move(candidate_viewers);
	collision_viewers_ = std::move(candidate_collision_viewers);
	current_plan_ = std::move(candidate_plan);
	for (const WtLodMapEntry &entry : transition_mask_updates) {
		const WtDesiredChunk *desired = desired_->find_desired(entry.key);
		if (desired != nullptr &&
			!publish_transition_mask_update(entry, *desired)) {
			if (!stop_requested_.load()) {
				set_failure(
					WtReadOnlyRuntimeStatus::PipelineTransitionMaskUpdateFailure
				);
			}
			return true;
		}
	}
	WtReadOnlyPublication plan_completed;
	plan_completed.kind = WtReadOnlyPublicationKind::ViewerPlanCompleted;
	plan_completed.world_revision = plan_snapshot.revision;
	if (!push_publication(std::move(plan_completed))) {
		if (!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
		return true;
	}
	plan_revision_ = plan_snapshot.revision;
	if (trace_enabled) {
		causal_trace_.record(
			WtCausalTraceEventKind::ViewerPlanApplied,
			WtCausalTraceThreadRole::Runtime,
			nullptr,
			{},
			event.snapshot.revision,
			planned_demand_count,
			wt_causal_trace_now_ns() - planning_started_ns
		);
	}
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
				true
			);
		if (collision_status != WtChunkResourceCacheStatus::Ok &&
			collision_status != WtChunkResourceCacheStatus::NotFound) {
			set_failure(
				WtReadOnlyRuntimeStatus::PipelineCollisionRebuildFailure
			);
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
		metrics_.foreground_priority_requested_keys +=
			foreground_overlay.requested_keys;
		metrics_.foreground_priority_matched_keys +=
			foreground_overlay.matched_keys;
		metrics_.foreground_priority_missing_keys +=
			foreground_overlay.missing_keys;
		metrics_.foreground_priority_changed_priorities +=
			foreground_overlay.changed_priorities;
	}
	process_pending_transition_remeshes();
	return true;
}
} // namespace world_transvoxel
