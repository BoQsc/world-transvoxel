#include "core/wt_chunk_key.h"
#include "streaming/wt_lod_map.h"
#include "streaming/wt_stream_scheduler.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace wt = world_transvoxel;

namespace {

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

void test_chunk_keys() {
	const wt::WtChunkKey negative = { -1, -2, -3, 0 };
	const wt::WtChunkBounds bounds = wt::wt_chunk_bounds(negative);
	check(bounds.minimum.x == -16 && bounds.maximum.x == 0, "negative X bounds mismatch");
	check(bounds.minimum.y == -32 && bounds.maximum.y == -16, "negative Y bounds mismatch");
	check(bounds.minimum.z == -48 && bounds.maximum.z == -32, "negative Z bounds mismatch");

	const wt::WtChunkKey parent = wt::wt_parent_chunk_key(negative);
	check(parent == wt::WtChunkKey{ -1, -1, -2, 1 }, "negative parent floor division mismatch");
	const wt::WtChunkKey minimum_parent = wt::wt_parent_chunk_key({
		std::numeric_limits<std::int32_t>::min(), 0, 0, 0
	});
	check(minimum_parent.x == std::numeric_limits<std::int32_t>::min() / 2,
		"minimum coordinate parent overflow");
	check(wt::wt_chunk_extent(0) == 16 && wt::wt_chunk_extent(3) == 128,
		"LOD extent mismatch");
	check(wt::wt_face_bit(wt::WtChunkFace::PositiveZ) == 32,
		"face mask encoding mismatch");
	check(wt::wt_opposite_face(wt::WtChunkFace::NegativeY) == wt::WtChunkFace::PositiveY,
		"opposite face mismatch");
}

void test_lod_map() {
	wt::WtLodMap map(16);
	std::vector<wt::WtChunkKey> same_lod = {
		{ 1, 0, 0, 0 },
		{ 0, 0, 0, 0 },
		{ 0, 1, 0, 0 },
	};
	check(map.set_active_chunks(same_lod) == wt::WtLodMapStatus::Ok,
		"same-LOD map rejected");
	for (const wt::WtLodMapEntry &entry : map.get_entries()) {
		check(entry.transition_mask == 0, "same-LOD map created transition");
	}

	const wt::WtChunkKey coarse = { 0, 0, 0, 1 };
	std::vector<wt::WtChunkKey> mixed = { coarse };
	for (int y = 0; y < 2; ++y) {
		for (int z = 0; z < 2; ++z) {
			mixed.push_back({ 2, y, z, 0 });
		}
	}
	check(map.set_active_chunks(mixed) == wt::WtLodMapStatus::Ok,
		"valid 2:1 map rejected");
	const wt::WtLodMapEntry *coarse_entry = map.find(coarse);
	check(coarse_entry != nullptr, "coarse map entry missing");
	check(coarse_entry != nullptr &&
		coarse_entry->transition_mask == wt::wt_face_bit(wt::WtChunkFace::PositiveX),
		"coarse transition ownership mismatch");
	for (const wt::WtLodMapEntry &entry : map.get_entries()) {
		if (entry.key != coarse) {
			check(entry.transition_mask == 0, "fine chunk incorrectly owns transition");
		}
	}

	std::vector<wt::WtChunkKey> corner = mixed;
	for (int x = 0; x < 2; ++x) {
		for (int z = 0; z < 2; ++z) {
			corner.push_back({ x, 2, z, 0 });
		}
	}
	check(map.set_active_chunks(corner) == wt::WtLodMapStatus::Ok,
		"two-face LOD corner rejected");
	coarse_entry = map.find(coarse);
	const std::uint8_t expected_corner_mask =
		wt::wt_face_bit(wt::WtChunkFace::PositiveX) |
		wt::wt_face_bit(wt::WtChunkFace::PositiveY);
	check(coarse_entry != nullptr && coarse_entry->transition_mask == expected_corner_mask,
		"two-face transition ownership mismatch");

	check(map.set_active_chunks({ coarse, { 0, 0, 0, 0 } }) ==
		wt::WtLodMapStatus::OverlappingLeaves, "overlapping octree leaves accepted");
	check(map.set_active_chunks({ { 0, 0, 0, 2 }, { 4, 0, 0, 0 } }) ==
		wt::WtLodMapStatus::LodDifferenceExceeded, "LOD difference greater than one accepted");
	check(map.set_active_chunks({ coarse, coarse }) == wt::WtLodMapStatus::DuplicateKey,
		"duplicate chunk key accepted");
	check(map.set_active_chunks({ { 0, 0, 0, 21 } }) == wt::WtLodMapStatus::InvalidKey,
		"invalid LOD accepted");
	wt::WtLodMap tiny_map(1);
	check(tiny_map.set_active_chunks(same_lod) == wt::WtLodMapStatus::CapacityExceeded,
		"LOD map capacity not enforced");
}

void complete_job(wt::WtStreamScheduler &scheduler, const wt::WtChunkJob &job, bool success) {
	check(scheduler.submit_completion({ job.key, job.generation, job.stage, success }) ==
		wt::WtSchedulerStatus::Ok, "completion submission failed");
	check(scheduler.apply_completions(1) == 1, "completion was not applied");
}

void test_scheduler_priority_and_lifecycle() {
	wt::WtStreamScheduler scheduler(4, 8, 4, 2);
	const wt::WtChunkKey low = { 0, 0, 0, 0 };
	const wt::WtChunkKey high = { 1, 0, 0, 0 };
	const wt::WtChunkKey middle = { 2, 0, 0, 0 };
	check(scheduler.request_chunk(low, 1, 1) == wt::WtSchedulerStatus::Ok,
		"low-priority request failed");
	check(scheduler.request_chunk(high, 1, 10) == wt::WtSchedulerStatus::Ok,
		"high-priority request failed");
	check(scheduler.request_chunk(middle, 1, 5) == wt::WtSchedulerStatus::Ok,
		"middle-priority request failed");

	wt::WtChunkJob job;
	check(scheduler.pop_job(job) && job.key == high, "highest priority did not run first");
	check(scheduler.pop_job(job) && job.key == middle, "middle priority did not run second");
	check(scheduler.pop_job(job) && job.key == low, "lowest priority did not run last");

	wt::WtStreamScheduler lifecycle(2, 4, 4, 1);
	check(lifecycle.request_chunk(low, 7, 3) == wt::WtSchedulerStatus::Ok,
		"lifecycle request failed");
	check(lifecycle.pop_job(job) && job.stage == wt::WtChunkJobStage::Sample,
		"sample job missing");
	complete_job(lifecycle, job, true);
	const wt::WtChunkRecord *record = lifecycle.find_record(low);
	check(record != nullptr && record->lifecycle == wt::WtChunkLifecycle::Meshing,
		"sample completion did not advance to meshing");
	check(lifecycle.pop_job(job) && job.stage == wt::WtChunkJobStage::Mesh,
		"mesh job missing");
	complete_job(lifecycle, job, true);
	record = lifecycle.find_record(low);
	check(record != nullptr && record->lifecycle == wt::WtChunkLifecycle::Ready,
		"mesh completion did not become ready");
	check(lifecycle.request_chunk(low, 7, 99) == wt::WtSchedulerStatus::AlreadyCurrent,
		"ready generation was redundantly queued");
}

void test_scheduler_stale_and_bounds() {
	const wt::WtChunkKey key = { -1, 0, 0, 0 };
	wt::WtStreamScheduler scheduler(1, 4, 1, 1);
	wt::WtChunkJob job;
	for (std::uint64_t revision = 1; revision <= 1000; ++revision) {
		check(scheduler.request_chunk(key, revision, 1) == wt::WtSchedulerStatus::Ok,
			"repeated bounded request failed");
		check(scheduler.pop_job(job), "bounded request job missing");
		check(scheduler.cancel_chunk(key) == wt::WtSchedulerStatus::Ok,
			"bounded cancellation failed");
		complete_job(scheduler, job, true);
	}
	check(scheduler.get_records().size() == 1, "record storage grew without bound");
	check(scheduler.queued_job_count() == 0 && scheduler.queued_completion_count() == 0,
		"queues retained stale work");
	check(scheduler.get_metrics().stale_results == 1000,
		"stale completion count mismatch");
	check(scheduler.get_metrics().cancellations == 1000,
		"cancellation count mismatch");

	check(scheduler.update_viewer({ 4, 1, 2, 3, 1 }) == wt::WtSchedulerStatus::Ok,
		"viewer insert failed");
	check(scheduler.update_viewer({ 4, 2, 3, 4, 1 }) ==
		wt::WtSchedulerStatus::StaleViewerSnapshot, "stale viewer update accepted");
	check(scheduler.update_viewer({ 5, 0, 0, 0, 1 }) ==
		wt::WtSchedulerStatus::ViewerCapacityExceeded, "viewer capacity not enforced");

	wt::WtStreamScheduler queues(2, 1, 1, 0);
	check(queues.request_chunk({ 0, 0, 0, 0 }, 1, 0) == wt::WtSchedulerStatus::Ok,
		"queue setup request failed");
	check(queues.request_chunk({ 1, 0, 0, 0 }, 1, 0) ==
		wt::WtSchedulerStatus::JobQueueFull, "full job queue accepted work");
	check(queues.get_records().size() == 1, "rejected request created a record");
	check(queues.submit_completion({}) == wt::WtSchedulerStatus::Ok,
		"completion queue setup failed");
	check(queues.submit_completion({}) == wt::WtSchedulerStatus::CompletionQueueFull,
		"full completion queue accepted work");
}

} // namespace

int main() {
	test_chunk_keys();
	test_lod_map();
	test_scheduler_priority_and_lifecycle();
	test_scheduler_stale_and_bounds();
	if (failure_count != 0) {
		std::fprintf(stderr, "M2_CORE_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M2_CORE_PASS lod_maps=6 stale_cycles=1000\n");
	return 0;
}
