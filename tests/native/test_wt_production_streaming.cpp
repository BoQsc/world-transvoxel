#include "services/wt_read_only_world_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_hash256.h"
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
			("wt_production_streaming_" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()
			));
	}
	~FixtureRoot() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
	std::filesystem::path path;
};

struct PublicationCounts {
	std::size_t expects = 0;
	std::size_t removals = 0;
	std::size_t renders = 0;
	std::size_t collisions = 0;
	std::size_t render_vertices = 0;
	std::size_t render_indices = 0;
};

bool collect_until(
	wt::WtReadOnlyWorldRuntime &runtime,
	PublicationCounts &counts,
	std::size_t expected_renders,
	std::size_t expected_collisions,
	std::vector<std::uint8_t> &evidence
) {
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(5);
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
					if (publication.render) {
						counts.render_vertices += publication.render->vertices.size();
						counts.render_indices += publication.render->indices.size();
						if (evidence.empty()) {
							append_u64(evidence, publication.render->vertices.size());
							append_u64(evidence, publication.render->indices.size());
						}
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
			counts.collisions >= expected_collisions) {
			return true;
		}
		if (!consumed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

wt::WtViewerSnapshot viewer(
	std::uint64_t id,
	std::uint64_t revision,
	double x,
	double y
) {
	return { id, x, y, 8.0, revision };
}

} // namespace

int main() {
	FixtureRoot fixture;
	std::filesystem::path world_path;
	check(wtt::wt_write_production_streaming_fixture(
		fixture.path, 7001, 12, world_path
	), "streaming fixture write failed");

	wt::WtAsyncStorageService storage({ 16, 16, wt::kWtMaximumContainerSize });
	check(storage.open(world_path, fixture.path) ==
		wt::WtAsyncStorageStatus::Ok,
		"streaming fixture open failed");
	check(storage.page_count() == 4 &&
		storage.has_page({ -1, 0, 0, 0 }) &&
		storage.has_page({ 2, 0, 0, 0 }) &&
		!storage.has_page({ 3, 0, 0, 0 }),
		"streaming page catalog mismatch");

	wt::WtRuntimeConfig config;
	config.active_chunk_capacity = 8;
	config.viewer_capacity = 2;
	config.demand_capacity_per_viewer = 125;
	config.storage_request_capacity = 16;
	config.storage_completion_capacity = 16;
	config.encoded_page_entry_capacity = 8;
	config.decoded_page_entry_capacity = 8;
	config.mesh_entry_capacity = 8;
	config.render_entry_capacity = 8;
	config.collision_entry_capacity = 8;
	wt::WtReadOnlyWorldRuntime runtime(config, storage);
	check(runtime.valid(), "read-only runtime configuration rejected");
	std::atomic<wt::WtReadOnlyRuntimeStatus> run_status {
		wt::WtReadOnlyRuntimeStatus::Ok
	};
	std::thread worker([&]() { run_status.store(runtime.run()); });

	check(runtime.update_viewer(viewer(1, 1, 8.0, 8.0), 0) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"initial viewer update rejected");
	check(runtime.update_viewer(viewer(9, 1, 8.0, 8.0), 3) ==
		wt::WtReadOnlyRuntimeStatus::InvalidViewer,
		"oversize viewer radius accepted");
	PublicationCounts counts;
	std::vector<std::uint8_t> evidence;
	check(collect_until(runtime, counts, 1, 1, evidence),
		"initial page did not publish render and collision");
	check(counts.expects == 1 && counts.render_vertices != 0 &&
		counts.render_indices != 0,
		"initial page publication mismatch");

	check(runtime.update_viewer(viewer(1, 2, 24.0, 8.0), 0) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"moving viewer update rejected");
	check(collect_until(runtime, counts, 2, 2, evidence),
		"moving viewer page did not publish");
	check(counts.removals >= 1 && counts.expects >= 2,
		"moving viewer did not evict and request");

	check(runtime.update_viewer(viewer(2, 1, 8.0, -24.0), 2) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"underground second viewer update rejected");
	check(collect_until(runtime, counts, 4, 4, evidence),
		"multi-viewer underground pages did not publish");

	check(runtime.update_viewer(viewer(2, 2, 8.0, 40.0), 2) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"vertical viewer update rejected");
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	check(runtime.remove_viewer(1, 3) == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.remove_viewer(2, 3) == wt::WtReadOnlyRuntimeStatus::Ok,
		"viewer removal rejected");
	const auto removal_deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(3);
	while (std::chrono::steady_clock::now() < removal_deadline &&
		runtime.get_metrics().viewer_removals != 2) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	wt::WtReadOnlyPublication publication;
	while (runtime.pop_publication(publication)) {
		if (publication.kind == wt::WtReadOnlyPublicationKind::RemoveChunk) {
			++counts.removals;
		}
	}

	runtime.request_stop();
	worker.join();
	storage.close();
	const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
	check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok,
		"read-only runtime did not stop cleanly");
	check(metrics.viewer_updates >= 4 && metrics.viewer_removals == 2 &&
		metrics.sample_jobs >= 4 && metrics.mesh_jobs >= 4 &&
		metrics.storage_completions >= 4 && metrics.mesh_completions >= 4,
		"read-only runtime metrics mismatch");
	append_u64(evidence, metrics.viewer_updates);
	append_u64(evidence, metrics.viewer_removals);
	append_u64(evidence, storage.page_count());

	if (failure_count != 0) {
		std::fprintf(stderr, "PRODUCTION_STREAMING_FAIL failures=%d\n",
			failure_count);
		return 1;
	}
	std::printf("PRODUCTION_STREAMING_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"PRODUCTION_STREAMING_PASS pages=4 viewers=2 movement=4 backend=MIT\n"
	);
	return 0;
}
