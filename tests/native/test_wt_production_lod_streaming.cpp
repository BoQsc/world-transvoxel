#include "services/wt_read_only_world_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_hash256.h"
#include "streaming/wt_balanced_lod_planner.h"
#include "wt_production_world_fixture.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

namespace wt = world_transvoxel;
namespace wtt = world_transvoxel::testing;

namespace {

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
	for (std::size_t index = 0; index < 8; ++index) {
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
	}
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

class FixtureRoot {
public:
	FixtureRoot() {
		path = std::filesystem::temp_directory_path() /
			("wt_production_lod_streaming_" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()
			));
	}
	~FixtureRoot() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
	std::filesystem::path path;
};

const wt::WtLodMapEntry *find_entry(
	const wt::WtBalancedLodPlan &plan,
	const wt::WtChunkKey &key
) {
	for (const wt::WtLodMapEntry &entry : plan.entries) {
		if (entry.key == key) return &entry;
	}
	return nullptr;
}

wt::WtLodPlannerViewer planner_viewer(
	std::uint64_t id,
	std::uint64_t revision,
	double x
) {
	return { { id, x, 8.0, 8.0, revision }, 1, 1 };
}

struct PublicationEvidence {
	std::size_t expects = 0;
	std::size_t removals = 0;
	std::size_t renders = 0;
	std::size_t collisions = 0;
	std::vector<std::uint64_t> bridge_generations;
	std::vector<std::uint64_t> bridge_vertices;
	std::vector<std::uint64_t> bridge_indices;
};

bool collect_until(
	wt::WtReadOnlyWorldRuntime &runtime,
	PublicationEvidence &counts,
	std::size_t expected_renders,
	std::size_t expected_collisions
) {
	const wt::WtChunkKey bridge { 1, 0, 0, 1 };
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(8);
	while (std::chrono::steady_clock::now() < deadline) {
		wt::WtReadOnlyPublication publication;
		bool consumed = false;
		while (runtime.pop_publication(publication)) {
			consumed = true;
			switch (publication.kind) {
				case wt::WtReadOnlyPublicationKind::ExpectChunk:
					++counts.expects;
					break;
				case wt::WtReadOnlyPublicationKind::RemoveChunk:
					++counts.removals;
					break;
				case wt::WtReadOnlyPublicationKind::RenderPayload:
					++counts.renders;
					if (publication.key == bridge && publication.render) {
						counts.bridge_generations.push_back(
							publication.generation.value
						);
						counts.bridge_vertices.push_back(
							publication.render->vertices.size()
						);
						counts.bridge_indices.push_back(
							publication.render->indices.size()
						);
					}
					break;
				case wt::WtReadOnlyPublicationKind::CollisionPayload:
					++counts.collisions;
					break;
				case wt::WtReadOnlyPublicationKind::SetCollisionRequired:
				case wt::WtReadOnlyPublicationKind::EditCommitted:
				case wt::WtReadOnlyPublicationKind::EditRejected:
				case wt::WtReadOnlyPublicationKind::AuthoritativeSampleReady:
				case wt::WtReadOnlyPublicationKind::AuthoritativeSampleRejected:
				case wt::WtReadOnlyPublicationKind::WorldSnapshotReady:
				case wt::WtReadOnlyPublicationKind::WorldSnapshotRejected:
					break;
			}
		}
		if (counts.renders >= expected_renders &&
			counts.collisions >= expected_collisions) return true;
		if (!consumed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

} // namespace

int main() {
	FixtureRoot fixture;
	std::filesystem::path world_path;
	check(wtt::wt_write_production_transition_fixture(
		fixture.path, 8001, 13, world_path
	), "transition fixture write failed");

	wt::WtAsyncStorageService storage({ 64, 64, wt::kWtMaximumContainerSize });
	check(storage.open(world_path, fixture.path) ==
		wt::WtAsyncStorageStatus::Ok,
		"transition fixture open failed");
	check(storage.page_count() == 28 &&
		storage.has_page({ 0, 0, 0, 1 }) &&
		storage.has_page({ 3, 0, 0, 1 }) &&
		storage.has_page({ 5, 1, 1, 0 }) &&
		!storage.has_page({ 6, 0, 0, 0 }),
		"transition page catalog mismatch");

	const std::vector<wt::WtLodPlannerViewer> first_viewer = {
		planner_viewer(1, 1, 8.0),
	};
	wt::WtBalancedLodPlanner planner(40, storage.page_keys());
	wt::WtBalancedLodPlan plan;
	check(planner.valid() && planner.plan(
		first_viewer, {}, {}, plan
	) == wt::WtBalancedLodPlannerStatus::Ok,
		"initial balanced LOD plan failed");
	const wt::WtLodMapEntry *bridge = find_entry(plan, { 1, 0, 0, 1 });
	check(plan.entries.size() == 9 && bridge != nullptr &&
		bridge->transition_mask == wt::wt_face_bit(wt::WtChunkFace::NegativeX),
		"initial balanced LOD topology mismatch");

	wt::WtBalancedLodPlanner bounded(8, storage.page_keys());
	wt::WtBalancedLodPlan rejected_plan;
	check(bounded.plan(first_viewer, {}, {}, rejected_plan) ==
		wt::WtBalancedLodPlannerStatus::CapacityExceeded,
		"active-capacity overflow was not rejected");
	check(planner.plan(
		{ first_viewer.front(), first_viewer.front() }, {}, {}, rejected_plan
	) == wt::WtBalancedLodPlannerStatus::DuplicateViewer,
		"duplicate planner viewer was not rejected");

	wt::WtRuntimeConfig config;
	config.active_chunk_capacity = 40;
	config.viewer_capacity = 2;
	config.demand_capacity_per_viewer = 125;
	config.storage_request_capacity = 64;
	config.storage_completion_capacity = 64;
	config.encoded_page_entry_capacity = 40;
	config.decoded_page_entry_capacity = 40;
	config.mesh_entry_capacity = 40;
	config.render_entry_capacity = 40;
	config.collision_entry_capacity = 40;
	wt::WtReadOnlyWorldRuntime runtime(config, storage);
	check(runtime.valid(), "multi-LOD runtime configuration rejected");
	std::atomic<wt::WtReadOnlyRuntimeStatus> run_status {
		wt::WtReadOnlyRuntimeStatus::Ok
	};
	std::thread worker([&]() { run_status.store(runtime.run()); });

	check(runtime.update_viewer({ 1, 8.0, 8.0, 8.0, 1 }, 1, 1) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"initial multi-LOD viewer was rejected");
	check(runtime.update_viewer({ 9, 8.0, 8.0, 8.0, 1 }, 1, 21) ==
		wt::WtReadOnlyRuntimeStatus::InvalidViewer,
		"invalid maximum LOD was accepted");
	PublicationEvidence publications;
	check(collect_until(runtime, publications, 9, 9),
		"initial multi-LOD plan did not publish all chunks");
	check(publications.bridge_generations.size() == 1 &&
		publications.bridge_vertices.front() != 0 &&
		publications.bridge_indices.front() != 0,
		"initial transition bridge geometry was not published");

	check(runtime.update_viewer({ 2, 80.0, 8.0, 8.0, 1 }, 1, 1) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"second multi-LOD viewer was rejected");
	check(collect_until(runtime, publications, 19, 19),
		"second viewer did not publish balanced transition chunks");
	check(publications.bridge_generations.size() == 2 &&
		publications.bridge_generations[0] !=
			publications.bridge_generations[1] &&
		publications.removals >= 1 && publications.expects >= 19,
		"transition-mask change did not force bridge remeshing");

	check(runtime.update_viewer({ 1, 40.0, 8.0, 8.0, 2 }, 1, 1) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"moving multi-LOD viewer was rejected");
	check(collect_until(runtime, publications, 27, 27),
		"moving viewer did not complete balanced refinement");
	check(runtime.remove_viewer(1, 3) == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.remove_viewer(2, 2) == wt::WtReadOnlyRuntimeStatus::Ok,
		"multi-LOD viewer removal was rejected");
	const auto removal_deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(5);
	while (std::chrono::steady_clock::now() < removal_deadline &&
		runtime.get_metrics().viewer_removals != 2) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	runtime.request_stop();
	worker.join();
	storage.close();
	const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
	check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok,
		"multi-LOD runtime did not stop cleanly");
	check(metrics.viewer_updates == 3 && metrics.viewer_removals == 2 &&
		metrics.transition_mesh_completions >= 3 &&
		metrics.mesh_completions >= 27 && metrics.rejected_events == 0,
		"multi-LOD runtime metrics mismatch");

	std::vector<std::uint8_t> evidence;
	append_u64(evidence, plan.entries.size());
	append_u64(evidence, bridge == nullptr ? 0 : bridge->transition_mask);
	for (std::uint64_t value : publications.bridge_vertices) {
		append_u64(evidence, value);
	}
	for (std::uint64_t value : publications.bridge_indices) {
		append_u64(evidence, value);
	}
	append_u64(evidence, metrics.viewer_updates);
	append_u64(evidence, metrics.viewer_removals);
	append_u64(evidence, metrics.transition_mesh_completions);
	append_u64(evidence, storage.page_count());

	if (failure_count != 0) {
		std::fprintf(stderr, "PRODUCTION_LOD_STREAMING_FAIL failures=%d\n",
			failure_count);
		return 1;
	}
	std::printf(
		"PRODUCTION_LOD_STREAMING_EVIDENCE entries=%zu mask=%u "
		"bridge0=%llu/%llu bridge1=%llu/%llu transition_completions=%llu\n",
		plan.entries.size(),
		bridge == nullptr ? 0U : static_cast<unsigned int>(bridge->transition_mask),
		static_cast<unsigned long long>(publications.bridge_vertices[0]),
		static_cast<unsigned long long>(publications.bridge_indices[0]),
		static_cast<unsigned long long>(publications.bridge_vertices[1]),
		static_cast<unsigned long long>(publications.bridge_indices[1]),
		static_cast<unsigned long long>(metrics.transition_mesh_completions)
	);
	std::printf("PRODUCTION_LOD_STREAMING_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"PRODUCTION_LOD_STREAMING_PASS pages=28 viewers=2 transitions=3 backend=MIT\n"
	);
	return 0;
}
