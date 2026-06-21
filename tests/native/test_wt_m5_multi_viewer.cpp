#include "streaming/wt_multi_viewer_desired_set.h"
#include "storage/wt_hash256.h"

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

wt::WtViewerSnapshot viewer(
	std::uint64_t id,
	std::uint64_t revision,
	double x = 0.0,
	double y = 0.0,
	double z = 0.0
) {
	return { id, x, y, z, revision };
}

wt::WtViewerChunkDemand demand(
	const wt::WtChunkKey &key,
	std::int32_t priority,
	bool collision_required = false
) {
	return { key, priority, collision_required };
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
	for (std::size_t index = 0; index < 8; ++index) {
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
	}
}

void append_i32(std::vector<std::uint8_t> &bytes, std::int32_t value) {
	const std::uint32_t bits = static_cast<std::uint32_t>(value);
	for (std::size_t index = 0; index < 4; ++index) {
		bytes.push_back(static_cast<std::uint8_t>(bits >> (index * 8)));
	}
}

void append_desired(
	std::vector<std::uint8_t> &bytes,
	const wt::WtDesiredChunk &chunk
) {
	append_i32(bytes, chunk.key.x);
	append_i32(bytes, chunk.key.y);
	append_i32(bytes, chunk.key.z);
	bytes.push_back(chunk.key.lod);
	append_i32(bytes, chunk.priority);
	append_u64(bytes, chunk.supporter_count);
	bytes.push_back(chunk.collision_required ? 1 : 0);
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void test_union_and_deltas(std::vector<std::uint8_t> &evidence) {
	const wt::WtChunkKey a = { 0, 0, 0, 0 };
	const wt::WtChunkKey b = { 1, 0, 0, 0 };
	const wt::WtChunkKey c = { -1, 0, 0, 0 };
	const wt::WtChunkKey d = { 0, 1, 0, 0 };
	wt::WtMultiViewerDesiredSet desired({ 3, 4, 8, 6 });
	check(desired.valid(), "valid desired-set configuration was rejected");
	wt::WtDesiredSetDelta delta;
	check(
		desired.update_viewer(
			viewer(1, 1),
			{ demand(b, 5), demand(a, 10, true) },
			delta
		) == wt::WtMultiViewerDesiredSetStatus::Ok,
		"first viewer update failed"
	);
	check(
		delta.added.size() == 2 && delta.removed.empty() &&
			delta.updated.empty() && delta.added[0].key == a &&
			delta.added[1].key == b,
		"first viewer delta mismatch"
	);
	check(
		desired.update_viewer(
			viewer(2, 1, 64.0),
			{ demand(c, 7), demand(a, 20) },
			delta
		) == wt::WtMultiViewerDesiredSetStatus::Ok,
		"second viewer update failed"
	);
	const wt::WtDesiredChunk *shared = desired.find_desired(a);
	check(
		delta.added.size() == 1 && delta.added[0].key == c &&
			delta.updated.size() == 1 && delta.updated[0].key == a &&
			shared != nullptr && shared->priority == 20 &&
			shared->supporter_count == 2 && shared->collision_required,
		"multi-viewer priority union mismatch"
	);
	check(
		desired.update_viewer(
			viewer(2, 2, 80.0),
			{ demand(d, 8), demand(a, 3) },
			delta
		) == wt::WtMultiViewerDesiredSetStatus::Ok,
		"viewer movement update failed"
	);
	shared = desired.find_desired(a);
	check(
		delta.added.size() == 1 && delta.added[0].key == d &&
			delta.removed.size() == 1 && delta.removed[0] == c &&
			delta.updated.size() == 1 && delta.updated[0].key == a &&
			shared != nullptr && shared->priority == 10 &&
			shared->supporter_count == 2 && shared->collision_required,
		"viewer movement delta mismatch"
	);
	check(
		desired.remove_viewer(1, 2, delta) ==
			wt::WtMultiViewerDesiredSetStatus::Ok,
		"viewer removal failed"
	);
	shared = desired.find_desired(a);
	check(
		delta.removed.size() == 1 && delta.removed[0] == b &&
			delta.updated.size() == 1 && delta.updated[0].key == a &&
			shared != nullptr && shared->priority == 3 &&
			shared->supporter_count == 1 && !shared->collision_required &&
			desired.viewer_count() == 1 && desired.total_demand_count() == 2,
		"viewer removal union mismatch"
	);
	check(
		desired.update_viewer(
			viewer(2, 3, 80.0),
			{ demand(a, 3, true), demand(d, 8) },
			delta
		) == wt::WtMultiViewerDesiredSetStatus::Ok &&
			delta.added.empty() && delta.removed.empty() &&
			delta.updated.size() == 1 && delta.updated[0].key == a,
		"collision demand change did not emit an update"
	);
	check(
		desired.update_viewer(
			viewer(2, 4, 80.0),
			{ demand(a, 3, true), demand(d, 8) },
			delta
		) == wt::WtMultiViewerDesiredSetStatus::Ok &&
			delta.added.empty() && delta.removed.empty() && delta.updated.empty(),
		"unchanged demand set emitted work"
	);
	const wt::WtMultiViewerDesiredSetMetrics before_idle = desired.get_metrics();
	for (int iteration = 0; iteration < 1000; ++iteration) {
		check(desired.get_desired_chunks().size() == 2, "idle read changed union");
	}
	check(
		desired.get_metrics().union_rebuilds == before_idle.union_rebuilds,
		"idle reads rebuilt the desired set"
	);
	check(
		desired.update_viewer(viewer(2, 4), {}, delta) ==
			wt::WtMultiViewerDesiredSetStatus::StaleViewerRevision &&
		desired.remove_viewer(2, 4, delta) ==
			wt::WtMultiViewerDesiredSetStatus::StaleViewerRevision &&
		desired.remove_viewer(99, 1, delta) ==
			wt::WtMultiViewerDesiredSetStatus::ViewerNotFound,
		"stale or missing viewer event was accepted"
	);
	for (const wt::WtDesiredChunk &chunk : desired.get_desired_chunks()) {
		append_desired(evidence, chunk);
	}
	const wt::WtMultiViewerDesiredSetMetrics metrics = desired.get_metrics();
	append_u64(evidence, metrics.viewer_updates);
	append_u64(evidence, metrics.viewer_removals);
	append_u64(evidence, metrics.union_rebuilds);
	append_u64(evidence, metrics.demand_items_processed);
	append_u64(evidence, metrics.chunks_added);
	append_u64(evidence, metrics.chunks_removed);
	append_u64(evidence, metrics.chunks_updated);
	append_u64(evidence, metrics.rejected_events);
}

void test_validation_and_capacity() {
	wt::WtDesiredSetDelta delta;
	wt::WtMultiViewerDesiredSet invalid({ 0, 1, 1, 1 });
	check(!invalid.valid(), "zero viewer capacity was accepted");
	check(
		invalid.update_viewer(viewer(1, 1), {}, delta) ==
			wt::WtMultiViewerDesiredSetStatus::InvalidConfiguration,
		"invalid desired set accepted an update"
	);

	wt::WtMultiViewerDesiredSet bounded({ 1, 2, 2, 2 });
	const wt::WtChunkKey a = { 0, 0, 0, 0 };
	const wt::WtChunkKey b = { 1, 0, 0, 0 };
	check(
		bounded.update_viewer(viewer(1, 1), { demand(a, 1) }, delta) ==
			wt::WtMultiViewerDesiredSetStatus::Ok,
		"bounded viewer fixture failed"
	);
	check(
		bounded.update_viewer(viewer(2, 1), { demand(b, 1) }, delta) ==
			wt::WtMultiViewerDesiredSetStatus::ViewerCapacityExceeded &&
		bounded.viewer_count() == 1 && bounded.get_desired_chunks().size() == 1,
		"viewer overflow mutated desired state"
	);
	check(
		bounded.update_viewer(
			viewer(1, 2),
			{ demand(a, 1), demand(b, 2), demand({ 2, 0, 0, 0 }, 3) },
			delta
		) == wt::WtMultiViewerDesiredSetStatus::ViewerDemandCapacityExceeded,
		"per-viewer demand overflow was accepted"
	);
	check(
		bounded.update_viewer(
			viewer(1, 2),
			{ demand(a, 1), demand(a, 2) },
			delta
		) == wt::WtMultiViewerDesiredSetStatus::DuplicateDemand,
		"duplicate viewer demand was accepted"
	);
	check(
		bounded.update_viewer(
			viewer(1, 2),
			{ demand({ 0, 0, 0, 21 }, 1) },
			delta
		) == wt::WtMultiViewerDesiredSetStatus::InvalidDemand,
		"invalid demand key was accepted"
	);
	check(
		bounded.update_viewer(
			viewer(0, 1), {}, delta
		) == wt::WtMultiViewerDesiredSetStatus::InvalidViewer &&
		bounded.update_viewer(
			viewer(3, 1, std::numeric_limits<double>::quiet_NaN()),
			{},
			delta
		) == wt::WtMultiViewerDesiredSetStatus::InvalidViewer,
		"invalid viewer snapshot was accepted"
	);
	check(
		bounded.remove_viewer(0, 1, delta) ==
			wt::WtMultiViewerDesiredSetStatus::InvalidViewer &&
		bounded.remove_viewer(1, 0, delta) ==
			wt::WtMultiViewerDesiredSetStatus::InvalidViewer,
		"invalid viewer removal was accepted"
	);

	wt::WtMultiViewerDesiredSet total({ 2, 2, 2, 2 });
	check(
		total.update_viewer(
			viewer(1, 1), { demand(a, 1), demand(b, 1) }, delta
		) == wt::WtMultiViewerDesiredSetStatus::Ok &&
		total.update_viewer(
			viewer(2, 1), { demand({ 2, 0, 0, 0 }, 1) }, delta
		) == wt::WtMultiViewerDesiredSetStatus::TotalDemandCapacityExceeded &&
		total.viewer_count() == 1,
		"total demand overflow mutated state"
	);
	wt::WtMultiViewerDesiredSet desired_limit({ 1, 2, 2, 1 });
	check(
		desired_limit.update_viewer(
			viewer(1, 1), { demand(a, 1), demand(b, 1) }, delta
		) == wt::WtMultiViewerDesiredSetStatus::DesiredChunkCapacityExceeded &&
		desired_limit.viewer_count() == 0,
		"desired union overflow mutated state"
	);

	wt::WtMultiViewerDesiredSet overlap({ 1, 2, 2, 2 });
	check(
		overlap.update_viewer(
			viewer(1, 1),
			{ demand({ 0, 0, 0, 1 }, 1), demand({ 0, 0, 0, 0 }, 2) },
			delta
		) == wt::WtMultiViewerDesiredSetStatus::Ok &&
		overlap.get_desired_chunks().size() == 2,
		"overlapping prefetch demands were incorrectly rejected"
	);
}

void test_determinism_and_repeated_motion() {
	const wt::WtChunkKey a = { -1, 0, 0, 0 };
	const wt::WtChunkKey b = { 0, 0, 0, 0 };
	const wt::WtChunkKey c = { 1, 0, 0, 0 };
	wt::WtDesiredSetDelta delta;
	wt::WtMultiViewerDesiredSet first({ 2, 2, 4, 3 });
	wt::WtMultiViewerDesiredSet second({ 2, 2, 4, 3 });
	check(
		first.update_viewer(
			viewer(1, 1), { demand(a, 2), demand(b, 4) }, delta
		) == wt::WtMultiViewerDesiredSetStatus::Ok &&
		first.update_viewer(
			viewer(2, 1), { demand(c, 3), demand(b, 8) }, delta
		) == wt::WtMultiViewerDesiredSetStatus::Ok &&
		second.update_viewer(
			viewer(2, 1), { demand(b, 8), demand(c, 3) }, delta
		) == wt::WtMultiViewerDesiredSetStatus::Ok &&
		second.update_viewer(
			viewer(1, 1), { demand(b, 4), demand(a, 2) }, delta
		) == wt::WtMultiViewerDesiredSetStatus::Ok &&
		first.get_desired_chunks() == second.get_desired_chunks(),
		"viewer or demand order changed the desired union"
	);

	wt::WtMultiViewerDesiredSet moving({ 1, 2, 2, 2 });
	for (std::uint64_t revision = 1; revision <= 1000; ++revision) {
		const std::int32_t x = static_cast<std::int32_t>(revision);
		check(
			moving.update_viewer(
				viewer(1, revision, static_cast<double>(x * 16)),
				{ demand({ x, 0, 0, 0 }, 10), demand({ x + 1, 0, 0, 0 }, 5) },
				delta
			) == wt::WtMultiViewerDesiredSetStatus::Ok,
			"repeated viewer movement failed"
		);
		check(
			moving.viewer_count() == 1 &&
			moving.total_demand_count() == 2 &&
			moving.get_desired_chunks().size() == 2,
			"repeated movement grew bounded state"
		);
	}
	check(
		moving.get_metrics().union_rebuilds == 1000 &&
		moving.get_metrics().demand_items_processed == 2000,
		"repeated movement metrics mismatch"
	);
}

} // namespace

int main() {
	std::vector<std::uint8_t> evidence;
	test_union_and_deltas(evidence);
	test_validation_and_capacity();
	test_determinism_and_repeated_motion();
	if (failure_count != 0) {
		std::fprintf(stderr, "M5_MULTI_VIEWER_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M5_MULTI_VIEWER_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"M5_MULTI_VIEWER_PASS viewers=2 motion_cycles=1000 idle_reads=1000\n"
	);
	return 0;
}
