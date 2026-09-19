#include "physics/wt_collision_apply_queue.h"
#include "render/wt_render_apply_queue.h"
#include "services/wt_chunk_application.h"
#include "services/wt_chunk_resource_cache.h"
#include "services/wt_desired_set_runtime.h"
#include "storage/wt_container_format.h"
#include "storage/wt_storage_page_cache.h"
#include "streaming/wt_stream_scheduler.h"

#include <cstdio>
#include <memory>

namespace wt = world_transvoxel;

namespace {

class AcceptRenderSink final : public wt::WtRenderSink {
public:
	bool apply_render(const wt::WtRenderPayload &) override { return true; }
};

class AcceptCollisionSink final : public wt::WtCollisionSink {
public:
	bool apply_collision(const wt::WtCollisionPayload &) override { return true; }
};

bool check(bool condition, const char *message) {
	if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
	return condition;
}

bool make_ready(
	const wt::WtChunkKey &key,
	wt::WtStreamScheduler &scheduler,
	wt::WtChunkApplicationService &application,
	wt::WtChunkResourceCache &resource_cache,
	wt::WtGenerationToken &generation
) {
	wt::WtChunkJob job;
	if (!scheduler.pop_job(job) ||
			scheduler.submit_completion({
				key, job.generation, wt::WtChunkJobStage::Sample, true,
			}) != wt::WtSchedulerStatus::Ok ||
			scheduler.apply_completions(1) != 1 ||
			!scheduler.pop_job(job) ||
			scheduler.submit_completion({
				key, job.generation, wt::WtChunkJobStage::Mesh, true,
			}) != wt::WtSchedulerStatus::Ok ||
			scheduler.apply_completions(1) != 1) {
		return false;
	}
	const wt::WtChunkRecord *record = scheduler.find_record(key);
	if (record == nullptr || record->lifecycle != wt::WtChunkLifecycle::Ready) {
		return false;
	}
	generation = record->generation;
	auto render = std::make_shared<wt::WtRenderPayload>();
	render->key = key;
	render->generation = generation;
	render->world_origin = { 0, 0, 0 };
	AcceptRenderSink render_sink;
	AcceptCollisionSink collision_sink;
	return resource_cache.insert_render(render, generation) ==
			wt::WtChunkResourceCacheStatus::Ok &&
		application.submit_render(render) == wt::WtApplicationStatus::Ok &&
		application.apply(1, 0, render_sink, collision_sink).render_processed == 1;
}

} // namespace

int main() {
	const wt::WtChunkKey key = { 0, 0, 0, 0 };
	wt::WtStreamScheduler scheduler(4, 4, 4, 0);
	wt::WtChunkApplicationService application(4, 4, 4);
	wt::WtStoragePageCache page_cache({
		4, wt::kWtMaximumContainerSize,
		4, wt::kWtMaximumContainerSize,
	});
	wt::WtChunkResourceCache resource_cache({
		4, wt::kWtMaximumResourceCacheBytes,
		4, wt::kWtMaximumResourceCacheBytes,
		4, wt::kWtMaximumResourceCacheBytes,
	});
	wt::WtDesiredSetRuntimeService runtime(4);
	wt::WtDesiredSetDelta delta;
	delta.added = { { key, 10, 1, false } };
	if (!check(runtime.apply_delta(
			delta, 17, 5, scheduler, page_cache, resource_cache, application, nullptr
		) == wt::WtDesiredSetRuntimeStatus::Ok,
		"initial demand failed")) return 1;

	wt::WtGenerationToken generation;
	if (!check(make_ready(
			key, scheduler, application, resource_cache, generation
		), "fixture did not become fully ready")) return 1;
	// Atomic regional publication retires the application record before the
	// desired-set delta releases scheduler ownership. Dormant promotion must
	// accept that authoritative ordering when the immutable render payload is
	// still cached for the ready generation.
	if (!check(application.forget_chunk(key) == wt::WtApplicationStatus::Ok,
		"fixture application retirement failed")) return 1;

	delta.clear();
	delta.removed = { key };
	if (!check(runtime.apply_delta(
			delta, 17, 5, scheduler, page_cache, resource_cache, application, nullptr
		) == wt::WtDesiredSetRuntimeStatus::Ok &&
		scheduler.find_record(key) != nullptr &&
		application.find_record(key) == nullptr &&
		runtime.has_dormant_generation(key, generation) &&
		resource_cache.find_render(key, generation) != nullptr,
		"ready removal did not retain dormant residency")) return 1;

	delta.clear();
	delta.added = { { key, 25, 1, false } };
	if (!check(runtime.apply_delta(
			delta, 17, 5, scheduler, page_cache, resource_cache, application, nullptr
		) == wt::WtDesiredSetRuntimeStatus::Ok &&
		scheduler.find_record(key) != nullptr &&
		scheduler.find_record(key)->generation == generation &&
		scheduler.queued_job_count() == 0 &&
		application.find_record(key) != nullptr &&
		!runtime.has_dormant_generation(key, generation),
		"reactivation repeated work or changed generation")) return 1;
	AcceptRenderSink reactivation_render_sink;
	AcceptCollisionSink reactivation_collision_sink;
	const wt::WtApplicationBatchResult reactivation_apply = application.apply(
		1, 0, reactivation_render_sink, reactivation_collision_sink
	);
	wt::WtChunkApplicationRecord reactivated_record;
	if (!check(reactivation_apply.render_processed == 1 &&
			application.copy_record(key, reactivated_record) &&
			reactivated_record.visual_ready,
		"reactivation did not republish the cached visual")) return 1;

	const wt::WtDesiredSetRuntimeMetrics metrics = runtime.get_metrics();
	if (!check(metrics.dormant_insertions == 1 &&
			metrics.dormant_reactivations == 1 &&
			metrics.dormant_entries == 0,
		"dormant metrics mismatch")) return 1;

	std::printf(
		"DORMANT_RESIDENCY_PASS generation=%llu queued_jobs=%zu\n",
		static_cast<unsigned long long>(generation.value),
		scheduler.queued_job_count()
	);
	return 0;
}
