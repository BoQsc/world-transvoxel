#include "services/wt_read_only_world_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_edit_journal_store.h"
#include "storage/wt_hash256.h"
#include "storage/wt_procedural_world_source.h"
#include "streaming/wt_balanced_lod_planner.h"
#include "wt_production_world_fixture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
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

wt::WtId128 id(std::uint8_t seed) {
	wt::WtId128 output{};
	for (std::size_t index = 0; index < output.size(); ++index) {
		output[index] = static_cast<std::uint8_t>(seed + index);
	}
	return output;
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
	double x,
	std::uint32_t radius_chunks = 1,
	std::uint8_t maximum_lod = 1,
	std::uint32_t refinement_radius_chunks = 0
) {
	return {
		{ id, x, 8.0, 8.0, revision },
		radius_chunks,
		maximum_lod,
		refinement_radius_chunks,
	};
}

std::vector<wt::WtDesiredChunk> desired_from_plan(
	const wt::WtBalancedLodPlan &plan
) {
	std::vector<wt::WtDesiredChunk> desired;
	desired.reserve(plan.demands.size());
	for (const wt::WtViewerChunkDemand &demand : plan.demands) {
		desired.push_back({
			demand.key,
			demand.priority,
			1,
			demand.collision_required,
		});
	}
	return desired;
}

bool run_lod_hysteresis_regression() {
	std::vector<wt::WtChunkKey> keys;
	for (std::int32_t root_x = 0; root_x <= 2; ++root_x) {
		keys.push_back({ root_x, 0, 0, 1 });
		for (std::int32_t z = 0; z < 2; ++z) {
			for (std::int32_t y = 0; y < 2; ++y) {
				for (std::int32_t x = 0; x < 2; ++x) {
					keys.push_back({ root_x * 2 + x, y, z, 0 });
				}
			}
		}
	}
	wt::WtBalancedLodPlanner planner(64, std::move(keys));
	wt::WtBalancedLodPlan initial;
	wt::WtBalancedLodPlan fresh_boundary;
	wt::WtBalancedLodPlan retained_boundary;
	wt::WtBalancedLodPlan exited;
	const wt::WtChunkKey first_root = { 0, 0, 0, 1 };
	const std::vector<wt::WtLodPlannerViewer> initial_viewer = {
		planner_viewer(1, 1, 8.0, 2, 1, 1),
	};
	const std::vector<wt::WtLodPlannerViewer> boundary_viewer = {
		planner_viewer(1, 2, 50.0, 2, 1, 1),
	};
	const std::vector<wt::WtLodPlannerViewer> exited_viewer = {
		planner_viewer(1, 3, 70.0, 2, 1, 1),
	};
	check(
		planner.valid() &&
		planner.plan(initial_viewer, {}, {}, initial) ==
			wt::WtBalancedLodPlannerStatus::Ok &&
		find_entry(initial, first_root) == nullptr,
		"LOD hysteresis fixture did not start refined"
	);
	check(
		planner.plan(boundary_viewer, {}, {}, fresh_boundary) ==
			wt::WtBalancedLodPlannerStatus::Ok &&
		find_entry(fresh_boundary, first_root) != nullptr,
		"fresh LOD plan did not cross the refinement threshold"
	);
	check(
		planner.plan(
			boundary_viewer,
			desired_from_plan(initial),
			{},
			retained_boundary
		) == wt::WtBalancedLodPlannerStatus::Ok &&
		find_entry(retained_boundary, first_root) == nullptr,
		"LOD hysteresis did not retain the refined subtree"
	);
	check(
		planner.plan(
			exited_viewer,
			desired_from_plan(retained_boundary),
			{},
			exited
		) == wt::WtBalancedLodPlannerStatus::Ok &&
		find_entry(exited, first_root) != nullptr,
		"LOD hysteresis did not coarsen beyond its exit threshold"
	);
	return find_entry(initial, first_root) == nullptr &&
		find_entry(fresh_boundary, first_root) != nullptr &&
		find_entry(retained_boundary, first_root) == nullptr &&
		find_entry(exited, first_root) != nullptr;
}

double distance_to_bounds(
	double x,
	double y,
	double z,
	const wt::WtChunkBounds &bounds
) {
	const auto axis_distance = [](double value, std::int64_t minimum,
		std::int64_t maximum) {
		if (value < static_cast<double>(minimum)) {
			return static_cast<double>(minimum) - value;
		}
		if (value > static_cast<double>(maximum)) {
			return value - static_cast<double>(maximum);
		}
		return 0.0;
	};
	const double dx = axis_distance(x, bounds.minimum.x, bounds.maximum.x);
	const double dy = axis_distance(y, bounds.minimum.y, bounds.maximum.y);
	const double dz = axis_distance(z, bounds.minimum.z, bounds.maximum.z);
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool run_g21_near_field_capacity_regression(
	std::size_t &entry_count,
	double &nearest_coarse_distance
) {
	entry_count = 0;
	nearest_coarse_distance = 0.0;
	wt::WtProceduralWorldDescriptor descriptor;
	descriptor.chunk_count_x = 128;
	descriptor.chunk_count_y = 16;
	descriptor.chunk_count_z = 128;
	descriptor.chunk_y = -8;
	descriptor.source_revision = 190323;
	descriptor.seed = 19021;
	descriptor.mode = wt::WtProceduralWorldMode::RollingHillsCave;
	std::vector<wt::WtChunkKey> keys = wt::wt_procedural_keys(descriptor);
	check(
		keys.size() == 299520,
		"g21 near-field page hierarchy size mismatch"
	);
	wt::WtBalancedLodPlanner planner(8192, std::move(keys), 3, true);
	wt::WtBalancedLodPlan plan;
	const std::vector<wt::WtLodPlannerViewer> viewers = {
		{
			{ 1, 900.0, 58.0, 1030.0, 1 },
			10,
			3,
			0,
		},
	};
	const wt::WtBalancedLodPlannerStatus status = planner.plan(
		viewers, {}, {}, plan
	);
	check(
		planner.valid() && status == wt::WtBalancedLodPlannerStatus::Ok,
		"g21 three-chunk near-field plan exceeds production capacity"
	);
	if (status != wt::WtBalancedLodPlannerStatus::Ok) {
		return false;
	}
	entry_count = plan.entries.size();
	nearest_coarse_distance = std::numeric_limits<double>::infinity();
	std::int32_t minimum_collision_priority =
		std::numeric_limits<std::int32_t>::max();
	std::int32_t maximum_visual_only_priority =
		std::numeric_limits<std::int32_t>::min();
	bool saw_collision_demand = false;
	bool saw_visual_only_demand = false;
	for (const wt::WtViewerChunkDemand &demand : plan.demands) {
		if (demand.collision_required) {
			saw_collision_demand = true;
			minimum_collision_priority = std::min(
				minimum_collision_priority,
				demand.priority
			);
		} else {
			saw_visual_only_demand = true;
			maximum_visual_only_priority = std::max(
				maximum_visual_only_priority,
				demand.priority
			);
		}
	}
	for (const wt::WtLodMapEntry &entry : plan.entries) {
		if (entry.key.lod == 0) {
			continue;
		}
		nearest_coarse_distance = std::min(
			nearest_coarse_distance,
			distance_to_bounds(
				viewers.front().snapshot.x,
				viewers.front().snapshot.y,
				viewers.front().snapshot.z,
				wt::wt_chunk_bounds(entry.key)
			)
		);
	}
	check(
		entry_count <= 8192 && nearest_coarse_distance >= 48.0,
		"g21 near-field plan placed coarse terrain inside its safety radius"
	);
	check(
		saw_collision_demand && saw_visual_only_demand &&
		minimum_collision_priority > maximum_visual_only_priority,
		"player collision demand does not outrank visual-only streaming"
	);
	return entry_count <= 8192 && nearest_coarse_distance >= 48.0;
}

wt::WtEditTransaction carve_transaction(
	std::uint64_t source_revision,
	std::uint64_t base_revision,
	std::uint8_t seed,
	double center_x
) {
	const auto q16 = [](double value) {
		return static_cast<std::int64_t>(std::llround(
			value * static_cast<double>(wt::kWtEditCoordinateScale)
		));
	};
	wt::WtEditTransaction transaction;
	transaction.source_revision = source_revision;
	transaction.transaction_id = id(seed);
	transaction.base_revision = base_revision;
	transaction.committed_revision = base_revision + 1U;
	transaction.author_id = 41;
	wt::WtEditCommand command;
	command.command_id = id(static_cast<std::uint8_t>(seed + 64U));
	command.sequence = 0;
	command.world_revision = transaction.committed_revision;
	command.operation = wt::WtEditOperation::SdfCarve;
	command.shape = wt::WtEditShape::Sphere;
	command.density_value = 1.0F;
	command.sphere = {
		q16(center_x),
		q16(8.0),
		q16(8.0),
		static_cast<std::uint64_t>(2U) *
			static_cast<std::uint64_t>(wt::kWtEditCoordinateScale),
	};
	check(wt::wt_edit_sphere_bounds(command.sphere, command.bounds),
		"edit retention regression command bounds failed");
	transaction.commands.push_back(command);
	return transaction;
}

struct PublicationEvidence {
	std::size_t expects = 0;
	std::size_t removals = 0;
	std::size_t renders = 0;
	std::size_t collisions = 0;
	std::size_t staged_expects = 0;
	std::size_t staged_collision_preserve_expects = 0;
	std::vector<std::uint8_t> expect_remove_order;
	std::vector<std::uint64_t> bridge_staged_expect_generations;
	std::vector<std::uint64_t> bridge_preserved_collision_generations;
	std::vector<std::uint64_t> bridge_generations;
	std::vector<std::uint64_t> bridge_transition_masks;
	std::vector<std::uint64_t> bridge_vertices;
	std::vector<std::uint64_t> bridge_indices;
};

void drain_publications(wt::WtReadOnlyWorldRuntime &runtime) {
	wt::WtReadOnlyPublication publication;
	while (runtime.pop_publication(publication)) {}
}

bool is_control_publication(wt::WtReadOnlyPublicationKind kind) noexcept {
	switch (kind) {
		case wt::WtReadOnlyPublicationKind::ExpectChunk:
		case wt::WtReadOnlyPublicationKind::SetCollisionRequired:
		case wt::WtReadOnlyPublicationKind::SetVisualRequired:
		case wt::WtReadOnlyPublicationKind::RemoveChunk:
		case wt::WtReadOnlyPublicationKind::CollisionPayload:
			return true;
		case wt::WtReadOnlyPublicationKind::RenderPayload:
		case wt::WtReadOnlyPublicationKind::EditCommitted:
		case wt::WtReadOnlyPublicationKind::EditRejected:
		case wt::WtReadOnlyPublicationKind::AuthoritativeSampleReady:
		case wt::WtReadOnlyPublicationKind::AuthoritativeSampleRejected:
		case wt::WtReadOnlyPublicationKind::AuthoritativeSampleBatchReady:
		case wt::WtReadOnlyPublicationKind::AuthoritativeSampleBatchRejected:
		case wt::WtReadOnlyPublicationKind::WorldSnapshotReady:
		case wt::WtReadOnlyPublicationKind::WorldSnapshotRejected:
			return false;
	}
	return false;
}

bool runtime_idle(const wt::WtReadOnlyRuntimeMetrics &metrics) noexcept {
	return metrics.scheduler_queued_jobs == 0 &&
		metrics.scheduler_queued_completions == 0 &&
		metrics.scheduler_failed_records == 0 &&
		metrics.page_sample_failures == 0 &&
		metrics.page_mesh_failures == 0 &&
		metrics.page_storage_failures == 0 &&
		metrics.page_cache_failures == 0;
}

template <typename Predicate>
bool wait_for_runtime(
	wt::WtReadOnlyWorldRuntime &runtime,
	Predicate predicate
) {
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(8);
	while (std::chrono::steady_clock::now() < deadline) {
		drain_publications(runtime);
		if (predicate()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	drain_publications(runtime);
	return predicate();
}

template <typename Collector, typename Predicate>
bool collect_runtime_until(
	wt::WtReadOnlyWorldRuntime &runtime,
	Collector collector,
	Predicate predicate
) {
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(8);
	while (std::chrono::steady_clock::now() < deadline) {
		wt::WtReadOnlyPublication publication;
		bool consumed = false;
		while (runtime.pop_publication(publication)) {
			consumed = true;
			collector(publication);
		}
		if (predicate()) return true;
		if (!consumed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	wt::WtReadOnlyPublication publication;
	while (runtime.pop_publication(publication)) collector(publication);
	return predicate();
}

template <typename Collector, typename Predicate>
bool collect_runtime_until_quiescent(
	wt::WtReadOnlyWorldRuntime &runtime,
	Collector collector,
	Predicate predicate
) {
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(8);
	constexpr auto quiet_period = std::chrono::milliseconds(50);
	auto quiet_since = std::chrono::steady_clock::time_point{};
	while (std::chrono::steady_clock::now() < deadline) {
		wt::WtReadOnlyPublication publication;
		bool consumed = false;
		while (runtime.pop_publication(publication)) {
			consumed = true;
			collector(publication);
		}
		const auto now = std::chrono::steady_clock::now();
		if (!predicate() || consumed) {
			quiet_since = {};
		} else if (quiet_since == std::chrono::steady_clock::time_point{}) {
			quiet_since = now;
		} else if (now - quiet_since >= quiet_period) {
			return true;
		}
		if (!consumed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	wt::WtReadOnlyPublication publication;
	while (runtime.pop_publication(publication)) collector(publication);
	return predicate();
}

bool wait_for_viewer_update_idle(
	wt::WtReadOnlyWorldRuntime &runtime,
	std::uint64_t expected_viewer_updates
) {
	return wait_for_runtime(runtime, [&]() {
		const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
		return metrics.viewer_updates >= expected_viewer_updates &&
			runtime_idle(metrics);
	});
}

bool wait_for_edit_commit_idle(
	wt::WtReadOnlyWorldRuntime &runtime,
	std::uint64_t expected_revision,
	std::uint64_t expected_commits
) {
	return wait_for_runtime(runtime, [&]() {
		const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
		return runtime.world_revision() >= expected_revision &&
			metrics.edit_commits >= expected_commits &&
			runtime_idle(metrics);
	});
}

bool wait_for_edit_commit_visible(
	wt::WtReadOnlyWorldRuntime &runtime,
	std::uint64_t expected_revision,
	std::uint64_t expected_commits
) {
	return wait_for_runtime(runtime, [&]() {
		const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
		return runtime.world_revision() >= expected_revision &&
			metrics.edit_commits >= expected_commits;
	});
}

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
					counts.expect_remove_order.push_back(1);
					if (publication.staged_replacement) {
						++counts.staged_expects;
						if (publication.collision_required &&
								publication.preserve_collision_ready) {
							++counts.staged_collision_preserve_expects;
						}
						if (publication.key == bridge) {
							counts.bridge_staged_expect_generations.push_back(
								publication.generation.value
							);
							if (publication.collision_required &&
									publication.preserve_collision_ready) {
								counts.bridge_preserved_collision_generations.push_back(
									publication.generation.value
								);
							}
						}
					}
					break;
				case wt::WtReadOnlyPublicationKind::RemoveChunk:
					++counts.removals;
					counts.expect_remove_order.push_back(2);
					break;
				case wt::WtReadOnlyPublicationKind::RenderPayload:
					++counts.renders;
					if (publication.key == bridge && publication.render) {
						counts.bridge_generations.push_back(
							publication.generation.value
						);
						counts.bridge_transition_masks.push_back(
							publication.render->transition_mask
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
				case wt::WtReadOnlyPublicationKind::SetVisualRequired:
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

std::vector<std::size_t>
unique_bridge_render_indices(const PublicationEvidence &counts) {
	std::vector<std::size_t> indices;
	for (std::size_t index = 0; index < counts.bridge_generations.size();
			++index) {
		bool seen = false;
		for (const std::size_t previous : indices) {
			if (counts.bridge_generations[previous] ==
				counts.bridge_generations[index]) {
				seen = true;
				break;
			}
		}
		if (!seen) indices.push_back(index);
	}
	return indices;
}

bool run_collision_reactivation_eviction_regression(
	wt::WtAsyncStorageService &storage
) {
	const wt::WtChunkKey target { 0, 0, 0, 0 };
	wt::WtRuntimeConfig config;
	config.active_chunk_capacity = 40;
	config.viewer_capacity = 1;
	config.demand_capacity_per_viewer = 125;
	config.storage_request_capacity = 64;
	config.storage_completion_capacity = 64;
	config.encoded_page_entry_capacity = 40;
	config.decoded_page_entry_capacity = 40;
	config.mesh_entry_capacity = 40;
	config.render_entry_capacity = 40;
	config.collision_entry_capacity = 1;
	config.collision_activation_distance = 0.0;
	config.collision_deactivation_distance = 0.0;
	wt::WtReadOnlyWorldRuntime runtime(config, storage);
	check(runtime.valid(),
		"collision reactivation runtime configuration rejected");
	if (!runtime.valid()) return false;

	std::atomic<wt::WtReadOnlyRuntimeStatus> run_status {
		wt::WtReadOnlyRuntimeStatus::Ok
	};
	std::thread worker([&]() { run_status.store(runtime.run()); });
	bool initial_render = false;
	bool initial_collision = false;
	bool collision_deactivated = false;
	bool collision_reactivated = false;

	check(runtime.update_viewer({ 1, 8.0, 8.0, 8.0, 1 }, 2, 0) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"collision reactivation initial viewer was rejected");
	const bool initial_ready = collect_runtime_until(
		runtime,
		[&](const wt::WtReadOnlyPublication &publication) {
			if (publication.key != target) return;
			initial_render = initial_render ||
				publication.kind == wt::WtReadOnlyPublicationKind::RenderPayload;
			initial_collision = initial_collision ||
				publication.kind == wt::WtReadOnlyPublicationKind::CollisionPayload;
		},
		[&]() {
		return initial_render && initial_collision &&
			runtime.get_metrics().viewer_updates >= 1 &&
			runtime_idle(runtime.get_metrics());
		}
	);
	check(initial_ready,
		"collision reactivation target did not initially become ready");

	if (initial_ready) {
		check(runtime.update_viewer({ 1, 40.0, 8.0, 8.0, 2 }, 2, 0) ==
			wt::WtReadOnlyRuntimeStatus::Ok,
			"collision reactivation eviction viewer was rejected");
		const bool deactivated = collect_runtime_until(
			runtime,
			[&](const wt::WtReadOnlyPublication &publication) {
				if (publication.key == target && publication.kind ==
						wt::WtReadOnlyPublicationKind::SetCollisionRequired &&
					!publication.collision_required) {
					collision_deactivated = true;
				}
			},
			[&]() {
			return collision_deactivated &&
				runtime.get_metrics().viewer_updates >= 2 &&
				runtime_idle(runtime.get_metrics());
			}
		);
		check(deactivated,
			"collision reactivation target did not remain desired and deactivate");

		if (deactivated) {
			check(runtime.update_viewer({ 1, 8.0, 8.0, 8.0, 3 }, 2, 0) ==
				wt::WtReadOnlyRuntimeStatus::Ok,
				"collision reactivation return viewer was rejected");
			const bool reactivated = collect_runtime_until(
				runtime,
				[&](const wt::WtReadOnlyPublication &publication) {
					if (publication.key == target && publication.kind ==
							wt::WtReadOnlyPublicationKind::CollisionPayload) {
						collision_reactivated = true;
					}
				},
				[&]() {
				return collision_reactivated &&
					runtime.get_metrics().viewer_updates >= 3 &&
					runtime_idle(runtime.get_metrics());
				}
			);
			check(reactivated,
				"collision cache miss did not recover required collision");
		}
	}

	runtime.request_stop();
	worker.join();
	check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok,
		"collision reactivation runtime did not stop cleanly");
	return initial_render && initial_collision && collision_deactivated &&
		collision_reactivated &&
		run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok;
}

bool run_replacement_collision_continuity_regression(
	wt::WtAsyncStorageService &storage
) {
	wt::WtRuntimeConfig config;
	config.active_chunk_capacity = 40;
	config.viewer_capacity = 1;
	config.demand_capacity_per_viewer = 125;
	config.storage_request_capacity = 64;
	config.storage_completion_capacity = 64;
	config.encoded_page_entry_capacity = 40;
	config.decoded_page_entry_capacity = 40;
	config.mesh_entry_capacity = 40;
	config.render_entry_capacity = 40;
	config.collision_entry_capacity = 40;
	config.collision_activation_distance = 0.0;
	config.collision_deactivation_distance = 0.0;
	wt::WtReadOnlyWorldRuntime runtime(config, storage);
	check(runtime.valid(),
		"replacement collision-continuity runtime configuration rejected");
	if (!runtime.valid()) return false;

	std::atomic<wt::WtReadOnlyRuntimeStatus> run_status {
		wt::WtReadOnlyRuntimeStatus::Ok
	};
	std::thread worker([&]() { run_status.store(runtime.run()); });
	std::vector<wt::WtChunkKey> initial_visual_only;
	std::vector<wt::WtChunkKey> initial_rendered;
	std::vector<wt::WtChunkKey> initial_visible_visual_only;
	std::vector<wt::WtReadOnlyPublication> replacement_publications;
	const auto contains_key = [](const std::vector<wt::WtChunkKey> &keys,
								  const wt::WtChunkKey &key) {
		return std::find(keys.begin(), keys.end(), key) != keys.end();
	};
	const auto erase_key = [](std::vector<wt::WtChunkKey> &keys,
							   const wt::WtChunkKey &key) {
		keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
	};
	const auto add_key = [&](std::vector<wt::WtChunkKey> &keys,
							  const wt::WtChunkKey &key) {
		if (!contains_key(keys, key)) keys.push_back(key);
	};

	check(runtime.update_viewer({ 1, 8.0, 8.0, 8.0, 1 }, 1, 1) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"replacement collision-continuity initial viewer was rejected");
	const bool initial_ready = collect_runtime_until_quiescent(
		runtime,
		[&](const wt::WtReadOnlyPublication &publication) {
			if (publication.kind == wt::WtReadOnlyPublicationKind::ExpectChunk &&
				publication.visual_required &&
				!publication.collision_required) {
				add_key(initial_visual_only, publication.key);
				if (contains_key(initial_rendered, publication.key)) {
					add_key(initial_visible_visual_only, publication.key);
				}
			} else if (publication.kind ==
				wt::WtReadOnlyPublicationKind::RenderPayload) {
				add_key(initial_rendered, publication.key);
				if (contains_key(initial_visual_only, publication.key)) {
					add_key(initial_visible_visual_only, publication.key);
				}
			}
		},
		[&]() {
			const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
			return metrics.viewer_updates >= 1 && runtime_idle(metrics) &&
				!initial_visible_visual_only.empty();
		}
	);
	check(initial_ready && !initial_visible_visual_only.empty(),
		"replacement collision-continuity fixture had no visible visual-only chunks");

	bool replacement_ready = false;
	if (initial_ready && !initial_visible_visual_only.empty()) {
		check(runtime.update_viewer({ 1, 40.0, 8.0, 8.0, 2 }, 1, 1) ==
			wt::WtReadOnlyRuntimeStatus::Ok,
			"replacement collision-continuity moving viewer was rejected");
		replacement_ready = collect_runtime_until_quiescent(
			runtime,
			[&](const wt::WtReadOnlyPublication &publication) {
				replacement_publications.push_back(publication);
			},
			[&]() {
				const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
				return metrics.viewer_updates >= 2 && runtime_idle(metrics);
			}
		);
		check(replacement_ready,
			"replacement collision-continuity move did not become idle");
	}

	bool saw_visual_only_removal = false;
	bool every_removal_had_collision_handoff = true;
	for (std::size_t removal_index = 0;
		removal_index < replacement_publications.size();
		++removal_index) {
		const wt::WtReadOnlyPublication &removal =
			replacement_publications[removal_index];
		if (removal.kind != wt::WtReadOnlyPublicationKind::RemoveChunk ||
			std::find(
				initial_visible_visual_only.begin(),
				initial_visible_visual_only.end(),
				removal.key
			) == initial_visible_visual_only.end()) continue;
		saw_visual_only_removal = true;
		bool requirement_before_removal = false;
		bool payload_before_removal = false;
		for (std::size_t index = 0; index < removal_index; ++index) {
			const wt::WtReadOnlyPublication &publication =
				replacement_publications[index];
			if (publication.key != removal.key) continue;
			requirement_before_removal = requirement_before_removal ||
				(publication.kind ==
					wt::WtReadOnlyPublicationKind::SetCollisionRequired &&
					publication.collision_required);
			payload_before_removal = payload_before_removal ||
				(publication.kind ==
					wt::WtReadOnlyPublicationKind::CollisionPayload &&
					publication.collision != nullptr);
		}
		every_removal_had_collision_handoff =
			every_removal_had_collision_handoff &&
			requirement_before_removal && payload_before_removal;
	}
	if (!saw_visual_only_removal) {
		std::fprintf(
			stderr,
			"DIAG: collision-continuity initial_visible=%zu "
			"replacement_publications=%zu\n",
			initial_visible_visual_only.size(),
			replacement_publications.size()
		);
		for (const wt::WtChunkKey &key : initial_visible_visual_only) {
			std::fprintf(
				stderr,
				"DIAG: initial_visual_only key=%d,%d,%d lod=%u\n",
				key.x,
				key.y,
				key.z,
				static_cast<unsigned int>(key.lod)
			);
		}
		for (const wt::WtReadOnlyPublication &publication :
			replacement_publications) {
			std::fprintf(
				stderr,
				"DIAG: publication kind=%u key=%d,%d,%d lod=%u "
				"collision=%d visual=%d\n",
				static_cast<unsigned int>(publication.kind),
				publication.key.x,
				publication.key.y,
				publication.key.z,
				static_cast<unsigned int>(publication.key.lod),
				publication.collision_required ? 1 : 0,
				publication.visual_required ? 1 : 0
			);
		}
	}
	check(saw_visual_only_removal,
		"replacement collision-continuity move removed no visual-only chunk");
	check(every_removal_had_collision_handoff,
		"outgoing visual-only chunk lacked collision before staged removal");

	std::vector<wt::WtChunkKey> rendered = initial_rendered;
	std::vector<wt::WtChunkKey> visual_only_expected = initial_visual_only;
	std::vector<wt::WtChunkKey> current_visual_only =
		initial_visible_visual_only;
	const auto erase_visual_only = [&](const wt::WtChunkKey &key) {
		erase_key(visual_only_expected, key);
		erase_key(current_visual_only, key);
	};
	const auto add_visual_only = [&](const wt::WtChunkKey &key) {
		add_key(visual_only_expected, key);
		if (contains_key(rendered, key)) {
			add_key(current_visual_only, key);
		}
	};
	for (const wt::WtReadOnlyPublication &publication :
		replacement_publications) {
		switch (publication.kind) {
			case wt::WtReadOnlyPublicationKind::ExpectChunk:
				if (publication.visual_required &&
					!publication.collision_required) {
					add_visual_only(publication.key);
				} else {
					erase_visual_only(publication.key);
				}
				break;
			case wt::WtReadOnlyPublicationKind::SetCollisionRequired:
				if (publication.collision_required) {
					erase_visual_only(publication.key);
				} else {
					add_visual_only(publication.key);
				}
				break;
			case wt::WtReadOnlyPublicationKind::SetVisualRequired:
				if (!publication.visual_required) {
					erase_visual_only(publication.key);
				}
				break;
			case wt::WtReadOnlyPublicationKind::RemoveChunk:
				erase_visual_only(publication.key);
				break;
			case wt::WtReadOnlyPublicationKind::RenderPayload:
				add_key(rendered, publication.key);
				if (contains_key(visual_only_expected, publication.key)) {
					add_key(current_visual_only, publication.key);
				}
				break;
			case wt::WtReadOnlyPublicationKind::CollisionPayload:
			case wt::WtReadOnlyPublicationKind::EditCommitted:
			case wt::WtReadOnlyPublicationKind::EditRejected:
			case wt::WtReadOnlyPublicationKind::AuthoritativeSampleReady:
			case wt::WtReadOnlyPublicationKind::AuthoritativeSampleRejected:
			case wt::WtReadOnlyPublicationKind::AuthoritativeSampleBatchReady:
			case wt::WtReadOnlyPublicationKind::AuthoritativeSampleBatchRejected:
			case wt::WtReadOnlyPublicationKind::WorldSnapshotReady:
			case wt::WtReadOnlyPublicationKind::WorldSnapshotRejected:
				break;
		}
	}
	check(!current_visual_only.empty(),
		"replacement collision-continuity move left no visual-only chunks");

	std::vector<wt::WtReadOnlyPublication> removal_only_publications;
	bool removal_only_ready = false;
	if (!current_visual_only.empty()) {
		check(runtime.remove_viewer(1, 3) == wt::WtReadOnlyRuntimeStatus::Ok,
			"replacement collision-continuity viewer removal was rejected");
		removal_only_ready = collect_runtime_until(
			runtime,
			[&](const wt::WtReadOnlyPublication &publication) {
				removal_only_publications.push_back(publication);
			},
			[&]() {
				const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
				return metrics.viewer_removals >= 1 && runtime_idle(metrics);
			}
		);
		check(removal_only_ready,
			"replacement collision-continuity removal did not become idle");
	}
	bool saw_removal_only_visual_chunk = false;
	bool removal_only_handoff_ok = true;
	for (std::size_t removal_index = 0;
		removal_index < removal_only_publications.size();
		++removal_index) {
		const wt::WtReadOnlyPublication &removal =
			removal_only_publications[removal_index];
		if (removal.kind != wt::WtReadOnlyPublicationKind::RemoveChunk ||
			std::find(
				current_visual_only.begin(),
				current_visual_only.end(),
				removal.key
			) == current_visual_only.end()) continue;
		saw_removal_only_visual_chunk = true;
		bool requirement_before_removal = false;
		bool payload_before_removal = false;
		for (std::size_t index = 0; index < removal_index; ++index) {
			const wt::WtReadOnlyPublication &publication =
				removal_only_publications[index];
			if (publication.key != removal.key) continue;
			requirement_before_removal = requirement_before_removal ||
				(publication.kind ==
					wt::WtReadOnlyPublicationKind::SetCollisionRequired &&
					publication.collision_required);
			payload_before_removal = payload_before_removal ||
				(publication.kind ==
					wt::WtReadOnlyPublicationKind::CollisionPayload &&
					publication.collision != nullptr);
		}
		removal_only_handoff_ok = removal_only_handoff_ok &&
			requirement_before_removal && payload_before_removal;
	}
	check(saw_removal_only_visual_chunk && removal_only_handoff_ok,
		"removal-only visual chunk lacked collision before staged retirement");

	runtime.request_stop();
	worker.join();
	check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok,
		"replacement collision-continuity runtime did not stop cleanly");
	return initial_ready && replacement_ready && removal_only_ready &&
		saw_visual_only_removal && every_removal_had_collision_handoff &&
		saw_removal_only_visual_chunk && removal_only_handoff_ok &&
		run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok;
}

bool run_collision_publication_priority_regression(
	wt::WtAsyncStorageService &storage
) {
	wt::WtRuntimeConfig config;
	config.active_chunk_capacity = 40;
	config.viewer_capacity = 1;
	config.demand_capacity_per_viewer = 125;
	config.storage_request_capacity = 64;
	config.storage_completion_capacity = 64;
	config.encoded_page_entry_capacity = 40;
	config.decoded_page_entry_capacity = 40;
	config.mesh_entry_capacity = 40;
	config.render_entry_capacity = 40;
	config.collision_entry_capacity = 40;
	config.collision_activation_distance = 0.0;
	config.collision_deactivation_distance = 0.0;
	wt::WtReadOnlyWorldRuntime runtime(config, storage);
	check(runtime.valid(),
		"collision-publication priority runtime configuration rejected");
	if (!runtime.valid()) return false;

	std::atomic<wt::WtReadOnlyRuntimeStatus> run_status {
		wt::WtReadOnlyRuntimeStatus::Ok
	};
	std::thread worker([&]() { run_status.store(runtime.run()); });
	check(runtime.update_viewer({ 1, 8.0, 8.0, 8.0, 1 }, 1, 1) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"collision-publication priority initial viewer was rejected");
	const auto initial_deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(8);
	while (std::chrono::steady_clock::now() < initial_deadline) {
		const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
		if (metrics.viewer_updates >= 1 && runtime_idle(metrics)) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const wt::WtReadOnlyRuntimeMetrics initial_metrics = runtime.get_metrics();
	check(initial_metrics.viewer_updates >= 1 && runtime_idle(initial_metrics),
		"collision-publication priority initial stream did not become idle");

	bool left_render_backlog = false;
	wt::WtReadOnlyPublication publication;
	while (runtime.pop_publication(publication)) {
		if (publication.kind == wt::WtReadOnlyPublicationKind::RenderPayload) {
			left_render_backlog = true;
			break;
		}
	}
	if (!left_render_backlog) {
		drain_publications(runtime);
		runtime.request_stop();
		worker.join();
		check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
			runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok,
			"collision-publication priority runtime did not stop cleanly");
		return run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
			runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok;
	}

	check(runtime.update_viewer({ 1, 40.0, 8.0, 8.0, 2 }, 1, 1) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"collision-publication priority moving viewer was rejected");
	const auto movement_deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds(8);
	while (std::chrono::steady_clock::now() < movement_deadline) {
		const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
		if (metrics.viewer_updates >= 2 && runtime_idle(metrics)) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const wt::WtReadOnlyRuntimeMetrics movement_metrics = runtime.get_metrics();
	check(movement_metrics.viewer_updates >= 2 && runtime_idle(movement_metrics),
		"collision-publication priority movement did not become idle");

	const bool popped_after_move = runtime.pop_publication(publication);
	const bool control_overtook_render = popped_after_move &&
		publication.kind != wt::WtReadOnlyPublicationKind::RenderPayload;
	check(control_overtook_render,
		"collision/control publication remained behind render backlog");
	bool render_broke_through_priority_pressure = false;
	bool still_had_control_pressure = false;
	for (std::size_t index = 0; index < 24U &&
			runtime.pop_publication(publication); ++index) {
		still_had_control_pressure = still_had_control_pressure ||
			is_control_publication(publication.kind);
		if (publication.kind == wt::WtReadOnlyPublicationKind::RenderPayload) {
			render_broke_through_priority_pressure = true;
			break;
		}
	}
	check(
		render_broke_through_priority_pressure,
		"render publication starved behind collision/control backlog"
	);
	check(
		still_had_control_pressure,
		"collision-publication priority fixture had no remaining priority pressure"
	);
	drain_publications(runtime);
	runtime.request_stop();
	worker.join();
	check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok,
		"collision-publication priority runtime did not stop cleanly");
	return left_render_backlog && control_overtook_render &&
		still_had_control_pressure &&
		render_broke_through_priority_pressure &&
		run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok;
}

bool run_collision_publication_coalescing_regression(
	wt::WtAsyncStorageService &storage
) {
	wt::WtRuntimeConfig config;
	config.active_chunk_capacity = 40;
	config.viewer_capacity = 1;
	config.demand_capacity_per_viewer = 125;
	config.storage_request_capacity = 64;
	config.storage_completion_capacity = 64;
	config.encoded_page_entry_capacity = 40;
	config.decoded_page_entry_capacity = 40;
	config.mesh_entry_capacity = 40;
	config.render_entry_capacity = 40;
	config.collision_entry_capacity = 40;
	config.collision_activation_distance = 0.0;
	config.collision_deactivation_distance = 0.0;
	wt::WtReadOnlyWorldRuntime runtime(config, storage);
	check(runtime.valid(),
		"collision-publication coalescing runtime configuration rejected");
	if (!runtime.valid()) return false;

	std::atomic<wt::WtReadOnlyRuntimeStatus> run_status {
		wt::WtReadOnlyRuntimeStatus::Ok
	};
	std::thread worker([&]() { run_status.store(runtime.run()); });
	check(runtime.update_viewer({ 1, 8.0, 8.0, 8.0, 1 }, 1, 1) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"collision-publication coalescing initial viewer was rejected");
	const bool initial_ready = collect_runtime_until(
		runtime,
		[](const wt::WtReadOnlyPublication &) {},
		[&]() {
			const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
			return metrics.viewer_updates >= 1 && runtime_idle(metrics);
		}
	);
	check(initial_ready,
		"collision-publication coalescing initial stream did not become idle");
	drain_publications(runtime);

	std::size_t collision_state_publications = 0;
	check(runtime.update_viewer({ 1, 9.0, 8.0, 8.0, 2 }, 1, 1) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"collision-publication coalescing movement was rejected");
	const bool movement_ready = collect_runtime_until(
		runtime,
		[&](const wt::WtReadOnlyPublication &publication) {
			if (publication.kind ==
					wt::WtReadOnlyPublicationKind::SetCollisionRequired) {
				++collision_state_publications;
			}
		},
		[&]() {
			const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
			return metrics.viewer_updates >= 2 && runtime_idle(metrics);
		}
	);
	check(movement_ready,
		"collision-publication coalescing movement did not become idle");
	check(collision_state_publications == 0,
		"priority-only viewer movement flooded collision-state publications");

	runtime.request_stop();
	worker.join();
	check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok,
		"collision-publication coalescing runtime did not stop cleanly");
	return initial_ready && movement_ready &&
		collision_state_publications == 0 &&
		run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok;
}

std::size_t edit_retention_fallback_capacity(
	const std::vector<wt::WtChunkKey> &page_keys
) {
	const wt::WtLodPlannerViewer real =
		planner_viewer(1, 4, 8.0, 1, 1, 0);
	const std::vector<wt::WtLodPlannerViewer> full_retention = {
		real,
		planner_viewer(0x8000000000000001ULL, 1, 48.0, 0, 1, 2),
		planner_viewer(0x8000000000000002ULL, 2, 88.0, 0, 1, 2),
		planner_viewer(0x8000000000000003ULL, 3, 8.0, 0, 1, 2),
	};
	const std::vector<wt::WtLodPlannerViewer> newest_retention = {
		real,
		planner_viewer(0x8000000000000003ULL, 3, 8.0, 0, 1, 2),
	};
	const std::vector<wt::WtLodPlannerViewer> reduced_retention = {
		real,
		planner_viewer(0x8000000000000001ULL, 1, 48.0, 0, 1, 1),
		planner_viewer(0x8000000000000002ULL, 2, 88.0, 0, 1, 1),
		planner_viewer(0x8000000000000003ULL, 3, 8.0, 0, 1, 1),
	};
	for (std::size_t capacity = 10; capacity <= 40; ++capacity) {
		wt::WtBalancedLodPlanner planner(capacity, page_keys);
		if (!planner.valid()) continue;
		wt::WtBalancedLodPlan base_plan;
		wt::WtBalancedLodPlan full_plan;
		wt::WtBalancedLodPlan newest_plan;
		wt::WtBalancedLodPlan reduced_plan;
		if (planner.plan({ real }, {}, {}, base_plan) !=
			wt::WtBalancedLodPlannerStatus::Ok) {
			continue;
		}
		const wt::WtBalancedLodPlannerStatus full_status =
			planner.plan(full_retention, {}, {}, full_plan);
		const wt::WtBalancedLodPlannerStatus newest_status =
			planner.plan(newest_retention, {}, {}, newest_plan);
		const wt::WtBalancedLodPlannerStatus reduced_status =
			planner.plan(reduced_retention, {}, {}, reduced_plan);
		if (full_status != wt::WtBalancedLodPlannerStatus::Ok &&
			(newest_status == wt::WtBalancedLodPlannerStatus::Ok ||
				reduced_status == wt::WtBalancedLodPlannerStatus::Ok)) {
			return capacity;
		}
	}
	return 0;
}

bool run_global_coarse_lod_coverage_regression(
	const std::vector<wt::WtChunkKey> &page_keys
) {
	const wt::WtLodPlannerViewer viewer =
		planner_viewer(1, 1, 8.0, 1, 1, 0);
	wt::WtBalancedLodPlan local_plan;
	wt::WtBalancedLodPlanner local_planner(64, page_keys);
	check(local_planner.valid() && local_planner.plan(
		{ viewer }, {}, {}, local_plan
	) == wt::WtBalancedLodPlannerStatus::Ok,
		"local balanced LOD coverage plan failed");
	check(find_entry(local_plan, { 3, 0, 0, 1 }) == nullptr,
		"local balanced LOD planner unexpectedly kept far coarse root");

	wt::WtBalancedLodPlan global_plan;
	wt::WtBalancedLodPlanner global_planner(64, page_keys, 0, true);
	check(global_planner.valid() && global_planner.plan(
		{ viewer }, {}, {}, global_plan
	) == wt::WtBalancedLodPlannerStatus::Ok,
		"global coarse balanced LOD coverage plan failed");
	check(find_entry(global_plan, { 3, 0, 0, 1 }) != nullptr,
		"global coarse LOD coverage did not keep far coarse root active");
	check(global_plan.entries.size() >= local_plan.entries.size(),
		"global coarse LOD coverage unexpectedly shrank active coverage");
	return failure_count == 0;
}

bool run_edit_retention_fallback_regression(
	wt::WtAsyncStorageService &storage,
	const std::filesystem::path &root
) {
	const std::size_t fallback_capacity =
		edit_retention_fallback_capacity(storage.page_keys());
	check(fallback_capacity != 0,
		"edit retention fallback capacity search failed");
	if (fallback_capacity == 0) return false;

	wt::WtEditJournalStore journal;
	const std::filesystem::path journal_path =
		root / "edit_retention_fallback.wtedit";
	check(journal.open(
		journal_path,
		storage.source_revision(),
		storage.world_revision()
	) == wt::WtEditJournalStoreStatus::Ok,
		"edit retention fallback journal open failed");
	if (!journal.is_open()) return false;

	wt::WtRuntimeConfig config;
	config.active_chunk_capacity = 256;
	config.viewer_capacity = 4;
	config.demand_capacity_per_viewer = 125;
	config.storage_request_capacity = 128;
	config.storage_completion_capacity = 128;
	config.encoded_page_entry_capacity = 128;
	config.decoded_page_entry_capacity = 128;
	config.mesh_entry_capacity = 128;
	config.render_entry_capacity = 128;
	config.collision_entry_capacity = 128;
	wt::WtReadOnlyWorldRuntime runtime(config, storage, &journal);
	check(runtime.valid(),
		"edit retention fallback runtime configuration rejected");
	if (!runtime.valid()) {
		journal.close();
		return false;
	}

	std::atomic<wt::WtReadOnlyRuntimeStatus> run_status {
		wt::WtReadOnlyRuntimeStatus::Ok
	};
	std::thread worker([&]() { run_status.store(runtime.run()); });
	bool ok = true;
	const double centers[] = { 48.0, 88.0, 8.0 };
	for (std::size_t index = 0; ok && index < 3; ++index) {
		if (runtime.update_viewer(
				{ 1, centers[index], 8.0, 8.0, index + 1U },
				1,
				1
			) != wt::WtReadOnlyRuntimeStatus::Ok) {
			check(false, "edit retention fallback viewer update rejected");
			ok = false;
			break;
		}
		if (!wait_for_viewer_update_idle(runtime, index + 1U)) {
			check(false, "edit retention fallback viewer update did not idle");
			ok = false;
			break;
		}
		const std::uint64_t base_revision = runtime.world_revision();
		const wt::WtEditTransaction transaction = carve_transaction(
			storage.source_revision(),
			base_revision,
			static_cast<std::uint8_t>(11U + index),
			centers[index]
		);
		if (runtime.submit_edit(transaction) != wt::WtReadOnlyRuntimeStatus::Ok) {
			check(false, "edit retention fallback edit submit rejected");
			ok = false;
			break;
		}
		if (!wait_for_edit_commit_idle(
				runtime,
				transaction.committed_revision,
				index + 1U
			)) {
			check(false, "edit retention fallback edit did not commit and idle");
			ok = false;
			break;
		}
	}
	if (ok && runtime.update_viewer({ 1, 8.0, 8.0, 8.0, 4 }, 1, 1) !=
			wt::WtReadOnlyRuntimeStatus::Ok) {
		check(false, "edit retention fallback final viewer update rejected");
		ok = false;
	}
	if (ok && !wait_for_viewer_update_idle(runtime, 4)) {
		check(false, "edit retention fallback final viewer update did not idle");
		ok = false;
	}
	drain_publications(runtime);
	const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
	check(metrics.edit_commits == 3 && metrics.edit_rejections == 0,
		"edit retention fallback edit metrics mismatch");
	check(metrics.edit_lod_retention_zones == 1,
		"edit retention fallback did not merge nearby edited zones");
	check(metrics.edit_lod_retention_active_viewers != 0,
		"edit retention fallback dropped all retention viewers");
	check(metrics.rejected_events == 0,
		"edit retention fallback rejected a viewer event");

	runtime.request_stop();
	worker.join();
	journal.close();
	check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok,
		"edit retention fallback runtime did not stop cleanly");
	return ok && metrics.edit_commits == 3 && metrics.edit_rejections == 0 &&
		metrics.edit_lod_retention_zones == 1 &&
		metrics.edit_lod_retention_active_viewers != 0 &&
		metrics.rejected_events == 0 &&
		run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok;
}

bool run_edit_retention_many_zone_regression(
	wt::WtAsyncStorageService &storage,
	const std::filesystem::path &root
) {
	constexpr std::size_t kEditCount = 96;
	wt::WtEditJournalStore journal;
	const std::filesystem::path journal_path =
		root / "edit_retention_many_zones.wtedit";
	check(journal.open(
		journal_path,
		storage.source_revision(),
		storage.world_revision()
	) == wt::WtEditJournalStoreStatus::Ok,
		"edit retention many-zone journal open failed");
	if (!journal.is_open()) return false;

	wt::WtRuntimeConfig config;
	config.active_chunk_capacity = 512;
	config.viewer_capacity = 4;
	config.demand_capacity_per_viewer = 1024;
	config.storage_request_capacity = 128;
	config.storage_completion_capacity = 128;
	config.encoded_page_entry_capacity = 128;
	config.decoded_page_entry_capacity = 128;
	config.mesh_entry_capacity = 128;
	config.render_entry_capacity = 128;
	config.collision_entry_capacity = 128;
	wt::WtReadOnlyWorldRuntime runtime(config, storage, &journal);
	check(runtime.valid(),
		"edit retention many-zone runtime configuration rejected");
	if (!runtime.valid()) {
		journal.close();
		return false;
	}

	std::atomic<wt::WtReadOnlyRuntimeStatus> run_status {
		wt::WtReadOnlyRuntimeStatus::Ok
	};
	std::thread worker([&]() { run_status.store(runtime.run()); });
	bool ok = true;
	for (std::size_t index = 0; ok && index < kEditCount; ++index) {
		const std::uint64_t base_revision = runtime.world_revision();
		const wt::WtEditTransaction transaction = carve_transaction(
			storage.source_revision(),
			base_revision,
			static_cast<std::uint8_t>(150U + index),
			8.0 + static_cast<double>(index) * 96.0
		);
		if (runtime.submit_edit(transaction) != wt::WtReadOnlyRuntimeStatus::Ok) {
			check(false, "edit retention many-zone edit submit rejected");
			ok = false;
			break;
		}
		if (!wait_for_edit_commit_idle(
				runtime,
				transaction.committed_revision,
				index + 1U
			)) {
			check(false, "edit retention many-zone revision did not advance");
			ok = false;
			break;
		}
	}

	const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
	check(metrics.edit_commits == kEditCount && metrics.edit_rejections == 0,
		"edit retention many-zone edit metrics mismatch");
	check(metrics.edit_lod_retention_zones == kEditCount,
		"edit retention many-zone cap regressed below 96 zones");
	if (ok && runtime.update_viewer({ 1, 8.0, 8.0, 8.0, 1 }, 1, 1) !=
			wt::WtReadOnlyRuntimeStatus::Ok) {
		check(false, "edit retention many-zone viewer update rejected");
		ok = false;
	}
	if (ok && !wait_for_viewer_update_idle(runtime, 1)) {
		check(false, "edit retention many-zone viewer update did not idle");
		ok = false;
	}
	drain_publications(runtime);
	const wt::WtReadOnlyRuntimeMetrics planned_metrics = runtime.get_metrics();
	check(planned_metrics.edit_lod_retention_active_viewers != 0,
		"edit retention many-zone retained zones were not planned");
	check(planned_metrics.edit_lod_retention_plans != 0,
		"edit retention many-zone did not record a retained plan");
	check(planned_metrics.rejected_events == 0,
		"edit retention many-zone rejected a viewer event");
	runtime.request_stop();
	worker.join();
	journal.close();
	check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok,
		"edit retention many-zone runtime did not stop cleanly");
	return ok && metrics.edit_commits == kEditCount &&
		metrics.edit_rejections == 0 &&
		metrics.edit_lod_retention_zones == kEditCount &&
		planned_metrics.edit_lod_retention_active_viewers != 0 &&
		planned_metrics.edit_lod_retention_plans != 0 &&
		planned_metrics.rejected_events == 0 &&
		run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok;
}

bool run_edit_viewer_update_second_edit_regression(
	wt::WtAsyncStorageService &storage,
	const std::filesystem::path &root
) {
	wt::WtEditJournalStore journal;
	const std::filesystem::path journal_path =
		root / "edit_viewer_update_second_edit.wtedit";
	check(journal.open(
		journal_path,
		storage.source_revision(),
		storage.world_revision()
	) == wt::WtEditJournalStoreStatus::Ok,
		"edit/viewer/update journal open failed");
	if (!journal.is_open()) return false;

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
	wt::WtReadOnlyWorldRuntime runtime(config, storage, &journal);
	check(runtime.valid(),
		"edit/viewer/update runtime configuration rejected");
	if (!runtime.valid()) {
		journal.close();
		return false;
	}

	std::atomic<wt::WtReadOnlyRuntimeStatus> run_status {
		wt::WtReadOnlyRuntimeStatus::Ok
	};
	std::thread worker([&]() { run_status.store(runtime.run()); });
	bool ok = true;
	PublicationEvidence publications;
	if (runtime.update_viewer({ 1, 8.0, 8.0, 8.0, 1 }, 1, 1) !=
			wt::WtReadOnlyRuntimeStatus::Ok) {
		check(false, "edit/viewer/update initial viewer rejected");
		ok = false;
	}
	if (ok && !collect_until(runtime, publications, 9, 9)) {
		check(false, "edit/viewer/update initial terrain did not load");
		ok = false;
	}
	if (ok) {
		const wt::WtEditTransaction transaction = carve_transaction(
			storage.source_revision(),
			runtime.world_revision(),
			91,
			8.0
		);
		if (runtime.submit_edit(transaction) != wt::WtReadOnlyRuntimeStatus::Ok) {
			check(false, "edit/viewer/update first edit submit rejected");
			ok = false;
		} else if (!wait_for_edit_commit_visible(
				runtime,
				transaction.committed_revision,
				1
			)) {
			check(false, "edit/viewer/update first edit did not commit");
			ok = false;
		}
	}
	if (ok && runtime.update_viewer({ 1, 40.0, 8.0, 8.0, 2 }, 1, 1) !=
			wt::WtReadOnlyRuntimeStatus::Ok) {
		check(false, "edit/viewer/update post-edit viewer rejected");
		ok = false;
	}
	if (ok && !wait_for_viewer_update_idle(runtime, 2)) {
		check(false, "edit/viewer/update post-edit viewer did not idle");
		ok = false;
	}
	if (ok) {
		const wt::WtEditTransaction transaction = carve_transaction(
			storage.source_revision(),
			runtime.world_revision(),
			92,
			40.0
		);
		if (runtime.submit_edit(transaction) != wt::WtReadOnlyRuntimeStatus::Ok) {
			check(false, "edit/viewer/update second edit submit rejected");
			ok = false;
		} else if (!wait_for_edit_commit_idle(
				runtime,
				transaction.committed_revision,
				2
			)) {
			check(false, "edit/viewer/update second edit did not commit");
			ok = false;
		}
	}
	const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
	check(metrics.edit_commits == 2 && metrics.edit_rejections == 0,
		"edit/viewer/update edit metrics mismatch");
	check(metrics.rejected_events == 0,
		"edit/viewer/update rejected a viewer event");
	runtime.request_stop();
	worker.join();
	journal.close();
	check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok,
		"edit/viewer/update runtime did not stop cleanly");
	return ok &&
		metrics.edit_commits == 2 &&
		metrics.edit_rejections == 0 &&
		metrics.rejected_events == 0 &&
		run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok;
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
	wt::WtBalancedLodPlan far_plan;
	check(planner.plan(
		{ planner_viewer(2, 1, 80.0) }, {}, {}, far_plan
	) == wt::WtBalancedLodPlannerStatus::Ok,
		"far balanced LOD plan failed");
	check(far_plan.entries.size() == 10,
		"far balanced LOD topology mismatch");
	wt::WtBalancedLodPlan returned_mid_plan;
	check(planner.plan(
		{ planner_viewer(1, 3, 40.0) },
		desired_from_plan(far_plan),
		{},
		returned_mid_plan
	) == wt::WtBalancedLodPlannerStatus::Ok,
		"returned-mid balanced LOD plan failed");
	check(returned_mid_plan.entries.size() == 24,
		"returned-mid LOD hysteresis topology mismatch");
	const wt::WtChunkKey retained_edit_key { 5, 0, 0, 0 };
	check(find_entry(plan, retained_edit_key) == nullptr,
		"far edited detail key was unexpectedly active before retention");

	std::vector<wt::WtLodPlannerViewer> retained_viewers = first_viewer;
	retained_viewers.push_back(planner_viewer(
		0x8000000000000001ULL, 2, 80.0
	));
	wt::WtBalancedLodPlan retained_plan;
	check(planner.plan(
		retained_viewers, {}, {}, retained_plan
	) == wt::WtBalancedLodPlannerStatus::Ok,
		"edit-retention balanced LOD plan failed");
	check(find_entry(retained_plan, retained_edit_key) != nullptr,
		"edit-retention viewer did not keep far edited LOD0 key active");
	const bool global_coarse_ok =
		run_global_coarse_lod_coverage_regression(storage.page_keys());
	const bool lod_hysteresis_ok = run_lod_hysteresis_regression();
	std::size_t g21_entry_count = 0;
	double g21_nearest_coarse_distance = 0.0;
	const bool g21_near_field_ok = run_g21_near_field_capacity_regression(
		g21_entry_count, g21_nearest_coarse_distance
	);

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

	const std::size_t removals_before_second_viewer = publications.removals;
	check(runtime.update_viewer({ 2, 80.0, 8.0, 8.0, 1 }, 1, 1) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"second multi-LOD viewer was rejected");
	check(collect_until(runtime, publications, 19, 19),
		"second viewer did not publish balanced transition chunks");
	const bool bridge_mask_only_update =
		publications.bridge_generations.size() >= 2 &&
		publications.bridge_transition_masks.size() ==
			publications.bridge_generations.size() &&
		publications.bridge_generations[0] == publications.bridge_generations[1] &&
		publications.bridge_transition_masks[0] !=
			publications.bridge_transition_masks[1] &&
		publications.removals == removals_before_second_viewer &&
		publications.bridge_staged_expect_generations.empty() &&
		publications.bridge_preserved_collision_generations.empty() &&
		publications.expects >= 18;
	if (!bridge_mask_only_update) {
		std::fprintf(
			stderr,
			"bridge mask-only evidence: renders=%zu staged=%zu "
			"preserved=%zu removals=%zu/%zu",
			publications.bridge_generations.size(),
			publications.bridge_staged_expect_generations.size(),
			publications.bridge_preserved_collision_generations.size(),
			publications.removals,
			removals_before_second_viewer
		);
		for (std::size_t index = 0;
				index < publications.bridge_generations.size();
				++index) {
			std::fprintf(
				stderr,
				" [%zu:g=%llu mask=%u v=%llu i=%llu]",
				index,
				static_cast<unsigned long long>(
					publications.bridge_generations[index]
				),
				static_cast<unsigned int>(
					publications.bridge_transition_masks[index]
				),
				static_cast<unsigned long long>(
					publications.bridge_vertices[index]
				),
				static_cast<unsigned long long>(
					publications.bridge_indices[index]
				)
			);
		}
		std::fprintf(stderr, "\n");
	}
	check(bridge_mask_only_update,
		"transition-mask change did not publish a cached bridge render "
		"variant without remeshing");

	const std::size_t order_before_moving_viewer =
		publications.expect_remove_order.size();
	check(runtime.update_viewer({ 1, 40.0, 8.0, 8.0, 2 }, 1, 1) ==
		wt::WtReadOnlyRuntimeStatus::Ok,
		"moving multi-LOD viewer was rejected");
	check(collect_until(runtime, publications, 27, 27),
		"moving viewer did not complete balanced refinement");
	bool moving_viewer_saw_expect = false;
	bool moving_viewer_saw_removal = false;
	bool moving_viewer_expect_after_removal = false;
	bool moving_viewer_seen_removal = false;
	for (std::size_t index = order_before_moving_viewer;
			index < publications.expect_remove_order.size();
			++index) {
		const std::uint8_t marker = publications.expect_remove_order[index];
		if (marker == 1) {
			moving_viewer_saw_expect = true;
			if (moving_viewer_seen_removal) {
				moving_viewer_expect_after_removal = true;
			}
		} else if (marker == 2) {
			moving_viewer_saw_removal = true;
			moving_viewer_seen_removal = true;
		}
	}
	check(moving_viewer_saw_expect && moving_viewer_saw_removal,
		"moving viewer did not exercise mixed addition/removal publications");
	check(!moving_viewer_expect_after_removal,
		"viewer delta published a removal before all additions were expected");
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
	const wt::WtReadOnlyRuntimeMetrics metrics = runtime.get_metrics();
	check(run_status.load() == wt::WtReadOnlyRuntimeStatus::Ok &&
		runtime.last_status() == wt::WtReadOnlyRuntimeStatus::Ok,
		"multi-LOD runtime did not stop cleanly");
	check(metrics.viewer_updates == 3 && metrics.viewer_removals == 2 &&
		metrics.transition_mesh_completions >= 2 &&
		metrics.mesh_completions >= 26 && metrics.rejected_events == 0,
		"multi-LOD runtime metrics mismatch");
	const bool fallback_retention_ok =
		run_edit_retention_fallback_regression(storage, fixture.path);
	const bool many_zone_retention_ok =
		run_edit_retention_many_zone_regression(storage, fixture.path);
	const bool edit_viewer_second_edit_ok =
		run_edit_viewer_update_second_edit_regression(storage, fixture.path);
	const bool collision_reactivation_ok =
		run_collision_reactivation_eviction_regression(storage);
	const bool replacement_collision_continuity_ok =
		run_replacement_collision_continuity_regression(storage);
	const bool collision_publication_priority_ok =
		run_collision_publication_priority_regression(storage);
	const bool collision_publication_coalescing_ok =
		run_collision_publication_coalescing_regression(storage);
	storage.close();

	std::vector<std::uint8_t> evidence;
	append_u64(evidence, plan.entries.size());
	append_u64(evidence, bridge == nullptr ? 0 : bridge->transition_mask);
	append_u64(evidence, retained_plan.entries.size());
	append_u64(evidence, find_entry(retained_plan, retained_edit_key) != nullptr);
	std::vector<std::size_t> bridge_render_indices;
	for (std::size_t index = 0;
			index < publications.bridge_generations.size() &&
				bridge_render_indices.size() < 2;
			++index) {
		bridge_render_indices.push_back(index);
	}
	for (const std::size_t index : bridge_render_indices) {
		append_u64(evidence, publications.bridge_transition_masks[index]);
		append_u64(evidence, publications.bridge_vertices[index]);
		append_u64(evidence, publications.bridge_indices[index]);
	}
	append_u64(evidence, publications.staged_expects);
	append_u64(evidence, publications.staged_collision_preserve_expects);
	for (std::uint64_t value : publications.bridge_staged_expect_generations) {
		append_u64(evidence, value);
	}
	for (std::uint64_t value : publications.bridge_preserved_collision_generations) {
		append_u64(evidence, value);
	}
	append_u64(evidence, metrics.viewer_updates);
	append_u64(evidence, metrics.viewer_removals);
	append_u64(evidence, metrics.transition_mesh_completions);
	append_u64(evidence, storage.page_count());
	append_u64(evidence, fallback_retention_ok ? 1U : 0U);
	append_u64(evidence, global_coarse_ok ? 1U : 0U);
	append_u64(evidence, many_zone_retention_ok ? 1U : 0U);
	append_u64(evidence, lod_hysteresis_ok ? 1U : 0U);
	append_u64(evidence, g21_near_field_ok ? 1U : 0U);
	append_u64(evidence, g21_entry_count);
	append_u64(evidence, edit_viewer_second_edit_ok ? 1U : 0U);
	append_u64(evidence, collision_reactivation_ok ? 1U : 0U);
	append_u64(evidence, replacement_collision_continuity_ok ? 1U : 0U);
	append_u64(evidence, collision_publication_priority_ok ? 1U : 0U);
	append_u64(evidence, collision_publication_coalescing_ok ? 1U : 0U);
	append_u64(
		evidence,
		static_cast<std::uint64_t>(g21_nearest_coarse_distance)
	);

	if (failure_count != 0) {
		std::fprintf(stderr, "PRODUCTION_LOD_STREAMING_FAIL failures=%d\n",
			failure_count);
		return 1;
	}
	std::printf(
		"PRODUCTION_LOD_STREAMING_EVIDENCE entries=%zu mask=%u "
		"retained_entries=%zu retained_edit_key=%d "
		"fallback_retention=%d bridge0=%llu/%llu bridge1=%llu/%llu "
		"staged_expects=%zu staged_collision_preserve_expects=%zu "
		"transition_stage_generations=%zu transition_preserve_generations=%zu "
		"global_coarse=%d many_zone_retention=%d lod_hysteresis=%d "
		"edit_viewer_second_edit=%d collision_reactivation=%d "
		"replacement_collision_continuity=%d "
		"collision_publication_priority=%d collision_publication_coalescing=%d "
		"g21_entries=%zu "
		"g21_nearest_coarse=%.1f transition_completions=%llu\n",
		plan.entries.size(),
		bridge == nullptr ? 0U : static_cast<unsigned int>(bridge->transition_mask),
		retained_plan.entries.size(),
		find_entry(retained_plan, retained_edit_key) != nullptr ? 1 : 0,
		fallback_retention_ok ? 1 : 0,
		static_cast<unsigned long long>(
			publications.bridge_vertices[bridge_render_indices[0]]
		),
		static_cast<unsigned long long>(
			publications.bridge_indices[bridge_render_indices[0]]
		),
		static_cast<unsigned long long>(
			publications.bridge_vertices[bridge_render_indices[1]]
		),
		static_cast<unsigned long long>(
			publications.bridge_indices[bridge_render_indices[1]]
		),
		publications.staged_expects,
		publications.staged_collision_preserve_expects,
		publications.bridge_staged_expect_generations.size(),
		publications.bridge_preserved_collision_generations.size(),
		global_coarse_ok ? 1 : 0,
		many_zone_retention_ok ? 1 : 0,
		lod_hysteresis_ok ? 1 : 0,
		edit_viewer_second_edit_ok ? 1 : 0,
		collision_reactivation_ok ? 1 : 0,
		replacement_collision_continuity_ok ? 1 : 0,
		collision_publication_priority_ok ? 1 : 0,
		collision_publication_coalescing_ok ? 1 : 0,
		g21_entry_count,
		g21_nearest_coarse_distance,
		static_cast<unsigned long long>(metrics.transition_mesh_completions)
	);
	std::printf("PRODUCTION_LOD_STREAMING_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"PRODUCTION_LOD_STREAMING_PASS pages=28 viewers=2 transitions=3 "
		"lod_hysteresis=1 collision_reactivation=1 "
		"edit_viewer_second_edit=1 "
		"replacement_collision_continuity=1 collision_publication_priority=1 "
		"collision_publication_coalescing=1 "
		"g21_near_field=1 backend=MIT\n"
	);
	return 0;
}
