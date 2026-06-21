#include "wt_m5_workload_fixture.h"

#include "physics/wt_collision_builder.h"
#include "render/wt_render_payload.h"

#include <algorithm>
#include <memory>

namespace world_transvoxel::testing {
namespace {

WtId128 make_id(std::uint64_t seed) {
	WtId128 value{};
	for (std::size_t index = 0; index < value.size(); ++index) {
		value[index] = static_cast<std::uint8_t>(seed + index * 29U);
	}
	return value;
}

WtCellVertex make_vertex(float x, float z, std::uint16_t material) {
	WtCellVertex result;
	result.position = { x, 0.0F, z };
	result.normal = { 0.0F, 1.0F, 0.0F };
	result.material = material;
	return result;
}

std::shared_ptr<const WtChunkMeshResult> make_mesh(const WtChunkKey &key) {
	auto mesh = std::make_shared<WtChunkMeshResult>();
	mesh->key = key;
	mesh->world_origin = wt_chunk_bounds(key).minimum;
	const std::uint16_t material = static_cast<std::uint16_t>(
		static_cast<std::uint32_t>(key.x) ^
		(static_cast<std::uint32_t>(key.y) << 3U) ^
		(static_cast<std::uint32_t>(key.z) << 7U)
	);
	mesh->regular.vertices = {
		make_vertex(0.0F, 0.0F, material),
		make_vertex(1.0F, 0.0F, material),
		make_vertex(0.0F, 1.0F, material),
	};
	mesh->regular.indices = { 0, 1, 2 };
	return mesh;
}

} // namespace

WtM5WorkloadFixture::WtM5WorkloadFixture() :
		desired_({ 4, 16, 64, kChunkCapacity }),
		scheduler_(kChunkCapacity, kQueueCapacity, kQueueCapacity, 4),
		application_(kChunkCapacity, kQueueCapacity, kQueueCapacity),
		page_cache_({
			kChunkCapacity,
			kWtMaximumContainerSize,
			kChunkCapacity,
			kWtMaximumContainerSize,
		}),
		resource_cache_({
			kChunkCapacity, kWtMaximumResourceCacheBytes,
			kChunkCapacity, kWtMaximumResourceCacheBytes,
			kChunkCapacity, kWtMaximumResourceCacheBytes,
		}),
		runtime_(64),
		edits_(kChunkCapacity),
		spatial_(kChunkCapacity, 4096, kChunkCapacity) {
	pending_.reserve(kChunkCapacity);
	update_maxima();
}

bool WtM5WorkloadFixture::valid() const noexcept {
	return desired_.valid() && page_cache_.valid() &&
		resource_cache_.valid() && runtime_.valid() && edits_.valid();
}

bool WtM5WorkloadFixture::update_viewer(
	const WtViewerSnapshot &snapshot,
	const std::vector<WtViewerChunkDemand> &demands
) {
	WtDesiredSetDelta delta;
	if (desired_.update_viewer(snapshot, demands, delta) !=
		WtMultiViewerDesiredSetStatus::Ok ||
		!apply_runtime_delta(delta)) {
		return false;
	}
	++metrics_.viewer_events;
	update_maxima();
	return true;
}

bool WtM5WorkloadFixture::remove_viewer(
	std::uint64_t viewer_id,
	std::uint64_t revision
) {
	WtDesiredSetDelta delta;
	if (desired_.remove_viewer(viewer_id, revision, delta) !=
		WtMultiViewerDesiredSetStatus::Ok ||
		!apply_runtime_delta(delta)) {
		return false;
	}
	++metrics_.viewer_events;
	update_maxima();
	return true;
}

bool WtM5WorkloadFixture::apply_runtime_delta(
	const WtDesiredSetDelta &delta
) {
	std::vector<WtChunkKey> collision_activations;
	collision_activations.reserve(delta.updated.size());
	for (const WtChunkKey &key : delta.removed) {
		remove_pending(key);
	}
	for (const WtDesiredChunk &item : delta.updated) {
		const WtChunkApplicationRecord *record = application_.find_record(item.key);
		if (record != nullptr && !record->collision_required &&
			item.collision_required) {
			collision_activations.push_back(item.key);
		}
	}
	if (runtime_.apply_delta(
			delta,
			kSourceRevision,
			world_revision_,
			scheduler_,
			page_cache_,
			resource_cache_,
			application_
		) != WtDesiredSetRuntimeStatus::Ok) {
		return false;
	}
	for (const WtDesiredChunk &item : delta.added) {
		const WtChunkRecord *record = scheduler_.find_record(item.key);
		if (record == nullptr) return false;
		mark_pending(item.key, record->generation);
	}
	for (const WtChunkKey &key : collision_activations) {
		const WtChunkRecord *record = scheduler_.find_record(key);
		if (record == nullptr) return false;
		mark_pending(key, record->generation);
		const auto collision =
			resource_cache_.find_collision(key, record->generation);
		if (collision &&
			application_.submit_collision(collision) != WtApplicationStatus::Ok) {
			return false;
		}
	}
	return true;
}

bool WtM5WorkloadFixture::apply_edit(const WtGridPoint &center) {
	std::vector<WtChunkKey> keys;
	keys.reserve(scheduler_.get_records().size());
	for (const WtChunkRecord &record : scheduler_.get_records()) {
		keys.push_back(record.key);
	}
	if (spatial_.rebuild(keys) != WtEditSpatialStatus::Ok) {
		return false;
	}
	WtEditTransaction transaction;
	transaction.source_revision = kSourceRevision;
	transaction.transaction_id = make_id(world_revision_ + 1);
	transaction.base_revision = world_revision_;
	transaction.committed_revision = world_revision_ + 1;
	transaction.author_id = 23;
	WtEditCommand command;
	command.command_id = make_id(world_revision_ + 101);
	command.world_revision = transaction.committed_revision;
	command.operation = WtEditOperation::AddDensity;
	command.shape = WtEditShape::Sphere;
	command.density_value = -0.25F;
	command.sphere = {
		center.x * kWtEditCoordinateScale,
		center.y * kWtEditCoordinateScale,
		center.z * kWtEditCoordinateScale,
		static_cast<std::uint64_t>(kWtEditCoordinateScale / 4),
	};
	if (!wt_edit_sphere_bounds(command.sphere, command.bounds)) {
		return false;
	}
	transaction.commands.push_back(command);
	if (edits_.replace_loaded_chunks(
			transaction,
			spatial_,
			scheduler_,
			page_cache_,
			resource_cache_,
			application_
		) != WtEditRuntimeReplacementStatus::Ok) {
		return false;
	}
	world_revision_ = transaction.committed_revision;
	for (const WtEditRuntimeReplacementRecord &replacement :
		edits_.get_last_replacements()) {
		mark_pending(replacement.key, replacement.replacement_generation);
	}
	++metrics_.edit_events;
	update_maxima();
	return true;
}

bool WtM5WorkloadFixture::publish_ready_chunk(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	const WtChunkApplicationRecord *application_record =
		application_.find_record(key);
	if (application_record == nullptr ||
		application_record->generation != generation) {
		return true;
	}
	const auto mesh = make_mesh(key);
	auto render = std::make_shared<WtRenderPayload>();
	auto collision = std::make_shared<WtCollisionPayload>();
	if (wt_build_render_payload(*mesh, generation, *render) !=
			WtRenderBuildStatus::Ok ||
		wt_build_collision_payload(*render, {}, *collision) !=
			WtCollisionBuildStatus::Ok ||
		resource_cache_.insert_mesh(mesh, generation, generation) !=
			WtChunkResourceCacheStatus::Ok ||
		resource_cache_.insert_render(render, generation) !=
			WtChunkResourceCacheStatus::Ok ||
		resource_cache_.insert_collision(collision, generation) !=
			WtChunkResourceCacheStatus::Ok ||
		application_.submit_render(render) != WtApplicationStatus::Ok) {
		return false;
	}
	if (application_record->collision_required &&
		application_.submit_collision(collision) != WtApplicationStatus::Ok) {
		return false;
	}
	return true;
}

bool WtM5WorkloadFixture::run_frame(
	std::size_t worker_budget,
	std::size_t render_budget,
	std::size_t collision_budget
) {
	update_maxima();
	WtChunkJob job;
	for (std::size_t count = 0;
		count < worker_budget && scheduler_.pop_job(job);
		++count) {
		++metrics_.worker_jobs;
		if (scheduler_.submit_completion({
				job.key,
				job.generation,
				job.stage,
				true,
			}) != WtSchedulerStatus::Ok ||
			scheduler_.apply_completions(1) != 1) {
			return false;
		}
		if (job.stage == WtChunkJobStage::Mesh) {
			const WtChunkRecord *record = scheduler_.find_record(job.key);
			if (record != nullptr && record->generation == job.generation &&
				record->lifecycle == WtChunkLifecycle::Ready &&
				!publish_ready_chunk(job.key, job.generation)) {
				return false;
			}
		}
	}
	application_.apply(
		render_budget,
		collision_budget,
		render_sink_,
		collision_sink_
	);
	++metrics_.frames;
	resolve_pending();
	update_maxima();
	return true;
}

bool WtM5WorkloadFixture::drain(std::size_t maximum_frames) {
	for (std::size_t frame = 0; frame < maximum_frames; ++frame) {
		bool ready = scheduler_.queued_job_count() == 0 &&
			scheduler_.queued_completion_count() == 0 &&
			application_.queued_render_count() == 0 &&
			application_.queued_collision_count() == 0;
		for (const WtChunkApplicationRecord &record : application_.get_records()) {
			ready = ready && record.fully_ready();
		}
		if (ready) return true;
		if (!run_frame(8, 4, 2)) return false;
	}
	return false;
}

void WtM5WorkloadFixture::mark_pending(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	remove_pending(key);
	pending_.push_back({ key, generation, metrics_.frames });
}

void WtM5WorkloadFixture::remove_pending(const WtChunkKey &key) {
	pending_.erase(
		std::remove_if(
			pending_.begin(),
			pending_.end(),
			[&key](const PendingReadiness &pending) {
				return pending.key == key;
			}
		),
		pending_.end()
	);
}

void WtM5WorkloadFixture::resolve_pending() {
	for (auto iterator = pending_.begin(); iterator != pending_.end();) {
		const WtChunkApplicationRecord *record =
			application_.find_record(iterator->key);
		if (record == nullptr || record->generation != iterator->generation) {
			iterator = pending_.erase(iterator);
			continue;
		}
		if (!record->fully_ready()) {
			++iterator;
			continue;
		}
		metrics_.maximum_readiness_latency_frames = std::max(
			metrics_.maximum_readiness_latency_frames,
			metrics_.frames - iterator->start_frame
		);
		iterator = pending_.erase(iterator);
	}
}

void WtM5WorkloadFixture::update_maxima() {
	metrics_.maximum_desired_chunks = std::max(
		metrics_.maximum_desired_chunks,
		desired_.get_desired_chunks().size()
	);
	metrics_.maximum_scheduler_records = std::max(
		metrics_.maximum_scheduler_records,
		scheduler_.get_records().size()
	);
	metrics_.maximum_job_queue = std::max(
		metrics_.maximum_job_queue,
		scheduler_.queued_job_count()
	);
	metrics_.maximum_render_queue = std::max(
		metrics_.maximum_render_queue,
		application_.queued_render_count()
	);
	metrics_.maximum_collision_queue = std::max(
		metrics_.maximum_collision_queue,
		application_.queued_collision_count()
	);
	const std::size_t resource_entries =
		resource_cache_.mesh_entry_count() +
		resource_cache_.render_entry_count() +
		resource_cache_.collision_entry_count();
	metrics_.maximum_resource_entries = std::max(
		metrics_.maximum_resource_entries,
		resource_entries
	);
}

bool WtM5WorkloadFixture::RenderSink::apply_render(
	const WtRenderPayload &
) {
	return true;
}

bool WtM5WorkloadFixture::CollisionSink::apply_collision(
	const WtCollisionPayload &
) {
	return true;
}

const WtMultiViewerDesiredSet &
WtM5WorkloadFixture::desired_set() const noexcept {
	return desired_;
}

const WtStreamScheduler &
WtM5WorkloadFixture::scheduler() const noexcept {
	return scheduler_;
}

const WtChunkApplicationService &
WtM5WorkloadFixture::application() const noexcept {
	return application_;
}

const WtChunkResourceCache &
WtM5WorkloadFixture::resource_cache() const noexcept {
	return resource_cache_;
}

const WtDesiredSetRuntimeService &
WtM5WorkloadFixture::runtime_service() const noexcept {
	return runtime_;
}

const WtEditRuntimeReplacementService &
WtM5WorkloadFixture::edit_service() const noexcept {
	return edits_;
}

WtM5WorkloadMetrics WtM5WorkloadFixture::get_metrics() const noexcept {
	return metrics_;
}

std::uint64_t WtM5WorkloadFixture::world_revision() const noexcept {
	return world_revision_;
}

} // namespace world_transvoxel::testing
