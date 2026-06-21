#include "services/wt_read_only_world_runtime.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "meshing/wt_chunk_mesher.h"
#include "physics/wt_collision_builder.h"
#include "render/wt_render_payload.h"
#include "services/wt_chunk_application.h"
#include "services/wt_chunk_resource_cache.h"
#include "services/wt_desired_set_runtime.h"
#include "services/wt_page_meshing_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_storage_page_cache.h"
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

bool chunk_coordinate(
	double position,
	std::int32_t &coordinate
) noexcept {
	if (!std::isfinite(position)) return false;
	const double value = std::floor(
		position / static_cast<double>(kWtChunkCellsPerAxis)
	);
	if (value < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
		value > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
		return false;
	}
	coordinate = static_cast<std::int32_t>(value);
	return true;
}

double axis_distance(double point, double minimum, double maximum) noexcept {
	if (point < minimum) return minimum - point;
	if (point > maximum) return point - maximum;
	return 0.0;
}

double distance_to_chunk(
	const WtViewerSnapshot &viewer,
	const WtChunkKey &key
) noexcept {
	const WtChunkBounds bounds = wt_chunk_bounds(key);
	const double dx = axis_distance(
		viewer.x,
		static_cast<double>(bounds.minimum.x),
		static_cast<double>(bounds.maximum.x)
	);
	const double dy = axis_distance(
		viewer.y,
		static_cast<double>(bounds.minimum.y),
		static_cast<double>(bounds.maximum.y)
	);
	const double dz = axis_distance(
		viewer.z,
		static_cast<double>(bounds.minimum.z),
		static_cast<double>(bounds.maximum.z)
	);
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

WtReadOnlyWorldRuntime::WtReadOnlyWorldRuntime(
	WtRuntimeConfig config,
	WtAsyncStorageService &storage
) : config_(config), storage_(storage) {
	if (wt_validate_runtime_config(config_) != WtRuntimeConfigStatus::Ok ||
		!storage_.is_open()) {
		last_status_.store(WtReadOnlyRuntimeStatus::InvalidConfiguration);
		return;
	}
	const std::size_t active = static_cast<std::size_t>(
		config_.active_chunk_capacity
	);
	const std::size_t viewers = static_cast<std::size_t>(config_.viewer_capacity);
	const std::size_t per_viewer = static_cast<std::size_t>(
		config_.demand_capacity_per_viewer
	);
	desired_ = std::make_unique<WtMultiViewerDesiredSet>(
		WtMultiViewerDesiredSetLimits {
			viewers,
			per_viewer,
			viewers * per_viewer,
			active,
		}
	);
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
	page_runtime_ = std::make_unique<WtPageMeshingRuntimeService>(active);
	mesher_ = std::make_unique<WtChunkMesher>(
		wt_get_transvoxel_mit_backend()
	);
	meshing_scratch_ = std::make_unique<WtChunkMeshingScratch>();
	viewer_event_capacity_ = std::max<std::size_t>(viewers * 2U, 2U);
	viewer_events_.reserve(viewer_event_capacity_);
	const std::size_t publication_capacity = std::max<std::size_t>(
		active * 4U,
		16U
	);
	publication_slots_.resize(publication_capacity);
	valid_ = desired_->valid() && page_cache_->valid() &&
		resource_cache_->valid() && desired_runtime_->valid() &&
		page_runtime_->valid();
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
	std::uint32_t radius_chunks
) {
	if (!valid_ || snapshot.id == 0 || snapshot.revision == 0 ||
		!std::isfinite(snapshot.x) || !std::isfinite(snapshot.y) ||
		!std::isfinite(snapshot.z) ||
		!valid_radius(radius_chunks, config_.demand_capacity_per_viewer)) {
		return WtReadOnlyRuntimeStatus::InvalidViewer;
	}
	return enqueue_viewer_event({
		ViewerEventKind::Update,
		snapshot,
		radius_chunks,
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
	return enqueue_viewer_event({ ViewerEventKind::Remove, snapshot, 0 }) ?
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

bool WtReadOnlyWorldRuntime::build_demands(
	const ViewerEvent &event,
	std::vector<WtViewerChunkDemand> &demands
) const {
	demands.clear();
	std::int32_t center_x = 0;
	std::int32_t center_y = 0;
	std::int32_t center_z = 0;
	if (!chunk_coordinate(event.snapshot.x, center_x) ||
		!chunk_coordinate(event.snapshot.y, center_y) ||
		!chunk_coordinate(event.snapshot.z, center_z)) {
		return false;
	}
	const std::int64_t radius = event.radius_chunks;
	const WtCollisionPolicy policy {
		kWtDefaultCollisionThinRatioSquared,
		config_.collision_activation_distance,
		config_.collision_deactivation_distance,
	};
	for (std::int64_t z = -radius; z <= radius; ++z) {
		for (std::int64_t y = -radius; y <= radius; ++y) {
			for (std::int64_t x = -radius; x <= radius; ++x) {
				const std::int64_t key_x = static_cast<std::int64_t>(center_x) + x;
				const std::int64_t key_y = static_cast<std::int64_t>(center_y) + y;
				const std::int64_t key_z = static_cast<std::int64_t>(center_z) + z;
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
				if (!storage_.has_page(key)) continue;
				const WtDesiredChunk *current = desired_->find_desired(key);
				const WtCollisionRequirement collision =
					wt_evaluate_collision_requirement(
						policy,
						current != nullptr && current->collision_required,
						distance_to_chunk(event.snapshot, key)
					);
				if (collision == WtCollisionRequirement::Invalid) return false;
				const std::int64_t distance_squared = x * x + y * y + z * z;
				demands.push_back({
					key,
					std::numeric_limits<std::int32_t>::max() -
						static_cast<std::int32_t>(distance_squared),
					collision == WtCollisionRequirement::Required,
				});
			}
		}
	}
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
	WtMultiViewerDesiredSet candidate = *desired_;
	WtDesiredSetDelta delta;
	WtMultiViewerDesiredSetStatus desired_status;
	std::vector<WtViewerChunkDemand> demands;
	if (event.kind == ViewerEventKind::Update) {
		if (!build_demands(event, demands)) {
			std::lock_guard<std::mutex> lock(metrics_mutex_);
			++metrics_.rejected_events;
			return true;
		}
		desired_status = candidate.update_viewer(
			event.snapshot,
			demands,
			delta
		);
	} else {
		desired_status = candidate.remove_viewer(
			event.snapshot.id,
			event.snapshot.revision,
			delta
		);
	}
	if (desired_status != WtMultiViewerDesiredSetStatus::Ok) {
		std::lock_guard<std::mutex> lock(metrics_mutex_);
		++metrics_.rejected_events;
		return true;
	}
	if (desired_runtime_->apply_delta(
			delta,
			storage_.source_revision(),
			storage_.world_revision(),
			*scheduler_,
			*page_cache_,
			*resource_cache_,
			*application_,
			page_runtime_.get()
		) != WtDesiredSetRuntimeStatus::Ok) {
		set_failure(WtReadOnlyRuntimeStatus::RuntimeDeltaFailure);
		return true;
	}
	*desired_ = std::move(candidate);
	if (!publish_delta(delta)) {
		if (!stop_requested_.load()) {
			set_failure(WtReadOnlyRuntimeStatus::PublicationFailure);
		}
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
			metrics_.planned_demands += demands.size();
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
			status = page_runtime_->begin_sample_job(
				job, 0, storage_, *page_cache_, *scheduler_
			);
			std::lock_guard<std::mutex> lock(metrics_mutex_);
			++metrics_.sample_jobs;
		} else {
			status = page_runtime_->execute_mesh_job(
				job, *mesher_, *meshing_scratch_, *scheduler_
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
	}
	return progressed;
}

} // namespace world_transvoxel
