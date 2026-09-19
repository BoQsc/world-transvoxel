#include "services/wt_chunk_publication_policy.h"
#include "services/wt_publication_spatial_index.h"
#include "services/wt_publication_dependency_graph.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <string>

namespace wt = world_transvoxel;
using Keys = std::vector<wt::WtChunkKey>;

namespace {
void check(bool ok, const char *message) {
	if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

void normalize(Keys &keys) {
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
}

bool contains(const Keys &keys, const wt::WtChunkKey &key) {
	return std::find(keys.begin(), keys.end(), key) != keys.end();
}

// Independent all-pairs oracle: integer bounds, no hierarchy lookup.
bool related(const wt::WtChunkKey &a, const wt::WtChunkKey &b, bool boundary) {
	const auto x = wt::wt_chunk_bounds(a);
	const auto y = wt::wt_chunk_bounds(b);
	const std::int64_t amin[] { x.minimum.x, x.minimum.y, x.minimum.z };
	const std::int64_t amax[] { x.maximum.x, x.maximum.y, x.maximum.z };
	const std::int64_t bmin[] { y.minimum.x, y.minimum.y, y.minimum.z };
	const std::int64_t bmax[] { y.maximum.x, y.maximum.y, y.maximum.z };
	int overlaps = 0, touches = 0;
	for (int i = 0; i < 3; ++i) {
		overlaps += amin[i] < bmax[i] && bmin[i] < amax[i];
		touches += amin[i] == bmax[i] || bmin[i] == amax[i];
	}
	return overlaps == 3 || (boundary && overlaps == 2 && touches == 1 &&
		std::abs(int(a.lod) - int(b.lod)) > 1);
}

wt::WtChunkPublicationRegion reference_region(
	const wt::WtChunkKey &seed, const Keys &replacements, const Keys &retirements
) {
	wt::WtChunkPublicationRegion region { {seed}, {} };
	bool changed = true;
	while (changed) {
		changed = false;
		for (const auto &old_key : retirements) {
			if (contains(region.retirements, old_key)) continue;
			for (const auto &new_key : region.replacements) {
				if (!related(new_key, old_key, true)) continue;
				region.retirements.push_back(old_key);
				changed = true;
				break;
			}
		}
		for (const auto &new_key : replacements) {
			if (contains(region.replacements, new_key)) continue;
			for (const auto &old_key : region.retirements) {
				if (!related(new_key, old_key, false)) continue;
				region.replacements.push_back(new_key);
				changed = true;
				break;
			}
		}
	}
	normalize(region.replacements);
	normalize(region.retirements);
	return region;
}

void compare(const Keys &replacements, const Keys &retirements) {
	for (const auto &seed : replacements) {
		const auto expected = reference_region(seed, replacements, retirements);
		wt::WtChunkPublicationRegion actual;
		check(wt::wt_build_chunk_publication_region(seed, replacements, retirements, actual),
			"valid publication region rejected");
		check(actual.replacements == expected.replacements && actual.retirements == expected.retirements,
			"indexed publication differs from integer all-pairs oracle");
	}
}

using Authority = std::function<bool(const wt::WtChunkKey &)>;

bool reference_covers(const wt::WtChunkKey &target, const Keys &keys, const Authority &authority) {
	if (!authority(target)) return true;
	const auto t = wt::wt_chunk_bounds(target);
	for (const auto &key : keys) {
		const auto b = wt::wt_chunk_bounds(key);
		if (b.minimum.x <= t.minimum.x && b.minimum.y <= t.minimum.y && b.minimum.z <= t.minimum.z &&
			b.maximum.x >= t.maximum.x && b.maximum.y >= t.maximum.y && b.maximum.z >= t.maximum.z) return true;
	}
	if (target.lod == 0) return false;
	for (int z = 0; z < 2; ++z) for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
		const std::int64_t cx = std::int64_t(target.x) * 2 + x;
		const std::int64_t cy = std::int64_t(target.y) * 2 + y;
		const std::int64_t cz = std::int64_t(target.z) * 2 + z;
		const auto valid = [](std::int64_t n) {
			return n >= std::numeric_limits<std::int32_t>::min() && n <= std::numeric_limits<std::int32_t>::max();
		};
		if (!valid(cx) || !valid(cy) || !valid(cz)) return false;
		const wt::WtChunkKey child {int(cx), int(cy), int(cz), std::uint8_t(target.lod - 1)};
		if (!authority(child)) continue;
		if (std::none_of(keys.begin(), keys.end(), [&](const auto &key) { return related(child, key, false); }) ||
			!reference_covers(child, keys, authority)) return false;
	}
	return true;
}

bool reference_coverage(const wt::WtChunkPublicationRegion &region, const Authority &authority) {
	if (!authority || region.replacements.empty() || region.retirements.empty()) return false;
	for (std::size_t i = 0; i < region.replacements.size(); ++i) {
		const auto &key = region.replacements[i];
		if (!wt::wt_is_valid_chunk_key(key) || !authority(key)) return false;
		for (std::size_t j = i + 1; j < region.replacements.size(); ++j) {
			if (related(key, region.replacements[j], false)) return false;
		}
	}
	for (const auto &key : region.retirements) {
		if (!wt::wt_is_valid_chunk_key(key) || !authority(key) || contains(region.replacements, key) ||
			!reference_covers(key, region.replacements, authority)) return false;
	}
	return true;
}

void compare_coverage(const wt::WtChunkPublicationRegion &region, const Authority &authority) {
	check(wt::wt_chunk_publication_region_has_complete_authoritative_coverage(region, authority) ==
		reference_coverage(region, authority), "indexed authoritative coverage differs from all-pairs oracle");
}

void coverage_regression() {
	std::mt19937 random(0xc0be);
	const Authority all = [](const auto &) { return true; };
	{
		// A coarse replacement can pull adjacent fine retirements into an atomic
		// unsafe-face swap. Existing desired coarse coverage on the far side must
		// participate in the proof without being rebuilt or reactivated.
		wt::WtChunkPublicationRegion region {
			{{3, 1, 2, 2}},
			{{16, 4, 10, 0}, {16, 5, 10, 0},
			 {16, 4, 11, 0}, {16, 5, 11, 0}},
		};
		check(!wt::wt_chunk_publication_region_has_complete_coverage(region),
			"unsafe-face retirements unexpectedly covered by new replacement");
		wt::wt_chunk_publication_region_append_retained_coverage(
			region, {{99, 99, 99, 0}, {2, 0, 1, 3}, {2, 0, 1, 3}}
		);
		check(region.replacements.size() == 2,
			"retained coverage admission was not bounded and unique");
		check(wt::wt_chunk_publication_region_has_complete_coverage(region),
			"retained active coverage did not close unsafe-face retirements");
	}
	const Authority clipped = [](const auto &key) {
		const auto b = wt::wt_chunk_bounds(key);
		return b.minimum.x < -16 && b.maximum.x > -96 && b.minimum.y < 112 && b.maximum.y > 16 &&
			b.minimum.z < -16 && b.maximum.z > -112;
	};
	const Authority sparse = [](const auto &key) {
		return related(key, {-7, 1, -6, 0}, false) || related(key, {-2, 6, -2, 0}, false);
	};
	for (int trial = 0; trial < 180; ++trial) {
		const wt::WtChunkKey root {-1, 0, -1, 3};
		Keys leaves {root};
		for (int split = 0; split < 60; ++split) {
			const auto i = random() % leaves.size();
			const auto parent = leaves[i];
			if (parent.lod == 0) continue;
			leaves.erase(leaves.begin() + i);
			for (int z = 0; z < 2; ++z) for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
				leaves.push_back({parent.x * 2 + x, parent.y * 2 + y, parent.z * 2 + z, std::uint8_t(parent.lod - 1)});
			}
		}
		for (const auto &authority : {all, clipped, sparse}) {
			wt::WtChunkPublicationRegion region {{}, {root}};
			for (const auto &key : leaves) if (authority(key)) region.replacements.push_back(key);
			check(reference_coverage(region, authority), "complete partition rejected by coverage oracle");
			compare_coverage(region, authority);
			std::shuffle(region.replacements.begin(), region.replacements.end(), random);
			compare_coverage(region, authority);
			const auto saved = region.replacements.back();
			region.replacements.push_back(saved); compare_coverage(region, authority);
			region.replacements.pop_back();
			region.replacements.push_back(wt::wt_parent_chunk_key(saved)); compare_coverage(region, authority);
			region.replacements.pop_back();
			region.replacements.pop_back(); compare_coverage(region, authority);
			region.replacements.push_back(saved);
			region.retirements.push_back(saved); compare_coverage(region, authority);
		}
	}
	for (const int limit : {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()}) {
		for (std::uint8_t lod : {0, 1, 19, 20}) {
			const wt::WtChunkKey child {limit, limit, limit, lod};
			if (lod < wt::kWtMaximumLod) {
				const auto parent = wt::wt_parent_chunk_key(child);
				compare_coverage({{parent}, {child}}, all);
				compare_coverage({{child}, {parent}}, all);
				compare_coverage({{parent, child}, {child}}, all);
			}
		}
	}
	compare_coverage({{{0, 0, 0, 21}}, {{0, 0, 0, 1}}}, all);
	compare_coverage({{{0, 0, 0, 0}}, {{0, 0, 0, 21}}}, all);
	compare_coverage({{}, {{0, 0, 0, 1}}}, all);
	compare_coverage({{{0, 0, 0, 0}}, {}}, all);
	compare_coverage({{{0, 0, 0, 0}}, {{0, 0, 0, 1}}}, {});
	std::cout << "AUTHORITATIVE_COVERAGE_PASS partition_cases=180 authorities=3 mutations=6 coordinate_limits=both\n";
}

void regression() {
	std::mt19937 random(0x7031);
	for (int trial = 0; trial < 250; ++trial) {
		Keys replacements, retirements;
		for (int i = 0; i < 70; ++i) {
			wt::WtChunkKey key { int(random() % 9) - 4, int(random() % 9) - 4,
				int(random() % 9) - 4, static_cast<std::uint8_t>(random() % 5) };
			(i % 2 == 0 ? replacements : retirements).push_back(key);
		}
		normalize(replacements); normalize(retirements);
		compare(replacements, retirements);
	}
	// Face contacts at both representable coordinate limits, all axes and LODs.
	for (std::uint8_t lod : {0, 1, 18, 19, 20}) {
		for (int axis = 0; axis < 3; ++axis) {
			for (const auto limit : {std::numeric_limits<std::int32_t>::min(),
					std::numeric_limits<std::int32_t>::max()}) {
				std::int32_t p[] {0, 0, 0}; p[axis] = limit;
				const wt::WtChunkKey seed {p[0], p[1], p[2], lod};
				Keys retirement;
				for (auto ancestor = seed; ancestor.lod < wt::kWtMaximumLod;) {
					ancestor = wt::wt_parent_chunk_key(ancestor);
					p[0] = ancestor.x; p[1] = ancestor.y; p[2] = ancestor.z;
					for (int offset : {-1, 0, 1}) {
						auto q = ancestor;
						if (axis == 0) q.x += offset;
						if (axis == 1) q.y += offset;
						if (axis == 2) q.z += offset;
						retirement.push_back(q);
					}
				}
				normalize(retirement); compare({seed}, retirement);
			}
		}
	}
	std::cout << "PUBLICATION_POLICY_PASS random_cases=250 coordinate_limit_cases=30\n";
}

void spatial_dependency_regression() {
	Keys retirements;
	for (int i = 0; i < 4096; ++i) retirements.push_back({i * 4, 0, 0, 3});
	const wt::WtPublicationSpatialIndex index(retirements);
	wt::WtPublicationSpatialIndex::QueryStats stats;
	Keys replacements;
	for (int z = 0; z < 2; ++z) for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
		const wt::WtChunkKey key {x, y, z, 2};
		replacements.push_back(key);
		Keys actual, expected;
		index.append_dependencies(key, true, actual, &stats);
		for (const auto &candidate : retirements) if (related(key, candidate, true)) expected.push_back(candidate);
		normalize(actual);
		check(actual == expected && index.overlaps(key), "spatial dependency query differs from integer oracle");
	}
	check(stats.tested_keys < 128, "local dependency query scanned unrelated retirements");
	normalize(replacements);
	compare(replacements, retirements);
	// Contacts across a fine-coordinate limit may still have representable
	// coarse neighbors. Querying integer bounds must not lose those faces.
	for (int limit : {std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
		for (std::uint8_t lod : {0, 1, 18, 20}) {
			const wt::WtChunkKey key {limit, limit, limit, lod};
			Keys candidates {key};
			for (auto parent = key; parent.lod < wt::kWtMaximumLod;) {
				parent = wt::wt_parent_chunk_key(parent);
				candidates.push_back(parent);
				candidates.push_back({parent.x + 1, parent.y, parent.z, parent.lod});
				candidates.push_back({parent.x - 1, parent.y, parent.z, parent.lod});
			}
			const wt::WtPublicationSpatialIndex limits(candidates);
			for (bool faces : {false, true}) {
				Keys actual, expected;
				limits.append_dependencies(key, faces, actual);
				for (const auto &candidate : candidates) if (related(key, candidate, faces)) expected.push_back(candidate);
				normalize(actual); normalize(expected);
				check(actual == expected, "coordinate-limit dependency query differs from oracle");
			}
		}
	}
	std::cout << "SPATIAL_PUBLICATION_DEPENDENCIES_PASS retirements=4096 queries=8 tested_keys="
		<< stats.tested_keys << " all_pairs_keys=32768 coordinate_limits=both\n";
}

void dependency_snapshot_regression() {
	wt::WtPublicationDependencyGraph graph;
	Keys replacements {{0, 0, 0, 0}}, retirements {{0, 0, 0, 1}};
	graph.update(replacements, retirements);
	graph.replacements(); graph.retirements();
	check(graph.index_builds() == 2, "initial spatial indexes were not constructed");
	for (int i = 0; i < 100; ++i) {
		graph.update(replacements, retirements);
		graph.replacements(); graph.retirements();
	}
	check(graph.index_builds() == 2, "unchanged publication membership rebuilt indexes");
	replacements.push_back({1, 0, 0, 0});
	graph.update(replacements, retirements);
	graph.replacements(); graph.retirements();
	check(graph.index_builds() == 3, "replacement mutation failed isolated invalidation");
	retirements.clear();
	graph.update(replacements, retirements);
	check(!graph.retirements().overlaps(replacements.front()) && graph.index_builds() == 4,
		"retirement removal retained stale spatial coverage");
	replacements.clear(); retirements = {{0, 0, 0, 1}};
	for (int z = 0; z < 2; ++z) for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
		replacements.push_back({x, y, z, 0});
	}
	normalize(replacements);
	for (int phase = 0; phase < 128; ++phase) {
		const auto lookup = [&](const wt::WtChunkKey &key, wt::WtGpuPublicationBoundary &boundary) {
			if (!contains(replacements, key)) return false;
			boundary = {std::uint8_t(phase / 2), phase % 2 != 0};
			return true;
		};
		wt::WtChunkPublicationRegion cached, fresh;
		Keys cached_wait, fresh_wait;
		const bool a = wt::wt_build_gpu_chunk_publication_cohort(replacements.front(), replacements,
			retirements, lookup, cached, cached_wait, 4096, &graph);
		const bool b = wt::wt_build_gpu_chunk_publication_cohort(replacements.front(), replacements,
			retirements, lookup, fresh, fresh_wait);
		check(a == b && cached.replacements == fresh.replacements && cached.retirements == fresh.retirements &&
			cached_wait == fresh_wait, "spatial snapshot cached changing masks or active-content readiness");
	}
	std::cout << "PUBLICATION_DEPENDENCY_SNAPSHOT_PASS unchanged_queries=100 mask_readiness_mutations=128 isolated_invalidation=1\n";
}

void sparse_fine_face_cohort_regression() {
	const wt::WtChunkKey coarse {0, 0, 0, 1};
	const wt::WtChunkKey fine {2, 0, 0, 0};
	Keys authoritative {coarse, fine};
	normalize(authoritative);
	wt::WtChunkPublicationRegion region;
	Keys waiting;
	const auto lookup = [&authoritative](
			const wt::WtChunkKey &key, wt::WtGpuPublicationBoundary &boundary) {
		if (!contains(authoritative, key)) return false;
		boundary = {0, false};
		return true;
	};
	check(wt::wt_build_gpu_chunk_publication_cohort(
		coarse, authoritative, {}, lookup, region, waiting, 64
	), "sparse authoritative fine face cohort was rejected");
	check(region.replacements.size() == 2 && contains(region.replacements, coarse) &&
		contains(region.replacements, fine),
		"GPU cohort invented absent fine-face siblings");
	std::cout << "GPU_SPARSE_FINE_FACE_COHORT_PASS authoritative=2 invented=0\n";
}

void cold_same_lod_cohort_regression() {
	const wt::WtChunkKey seed {0, 0, 0, 0};
	const wt::WtChunkKey edited_neighbor {1, 0, 0, 0};
	const wt::WtChunkKey cold_neighbor {2, 0, 0, 0};
	Keys candidates {seed, edited_neighbor, cold_neighbor};
	wt::WtChunkPublicationRegion region;
	Keys waiting;
	const auto cold_lookup = [&candidates](
			const wt::WtChunkKey &key, wt::WtGpuPublicationBoundary &boundary) {
		if (!contains(candidates, key)) return false;
		boundary = {0, false, true};
		return true;
	};
	check(wt::wt_build_gpu_chunk_publication_cohort(
		seed, candidates, {}, cold_lookup, region, waiting, 64
	), "cold same-LOD cohort was rejected");
	check(region.replacements == Keys {seed},
		"cold same-LOD candidates formed an unbounded publication chain");
	Keys cold_backlog;
	for (int z = -16; z < 16; ++z) {
		for (int x = -16; x < 16; ++x) cold_backlog.push_back({x, 0, z, 0});
	}
	normalize(cold_backlog);
	const auto backlog_lookup = [&cold_backlog](
			const wt::WtChunkKey &key, wt::WtGpuPublicationBoundary &boundary) {
		if (!contains(cold_backlog, key)) return false;
		boundary = {0, false, true};
		return true;
	};
	check(wt::wt_build_gpu_chunk_publication_cohort(
		seed, cold_backlog, {}, backlog_lookup, region, waiting, 8
	), "hot seed was rejected by a large cold same-LOD backlog");
	check(region.replacements == Keys {seed},
		"large cold backlog entered the hot seed publication cohort");
	const auto edit_lookup = [&candidates, &edited_neighbor](
			const wt::WtChunkKey &key, wt::WtGpuPublicationBoundary &boundary) {
		if (!contains(candidates, key)) return false;
		boundary = {0, false, key != edited_neighbor};
		return true;
	};
	check(wt::wt_build_gpu_chunk_publication_cohort(
		seed, candidates, {}, edit_lookup, region, waiting, 64
	), "same-LOD edit cohort was rejected");
	check(region.replacements == Keys({seed, edited_neighbor}),
		"same-LOD edited face neighbor was not retained atomically");
	std::cout << "GPU_COLD_SAME_LOD_COHORT_PASS cold_members=1 edit_members=2 backlog=1024 hot_members=1\n";
}

void same_key_replacement_retirement_regression() {
	const wt::WtChunkKey seed {3, 0, 1, 2};
	const Keys replacements {seed};
	const Keys retirements {seed};
	wt::WtChunkPublicationRegion region;
	Keys waiting;
	const auto lookup = [&seed](
			const wt::WtChunkKey &key, wt::WtGpuPublicationBoundary &boundary) {
		if (key != seed) return false;
		boundary = {0, false, false};
		return true;
	};
	check(wt::wt_build_gpu_chunk_publication_cohort(
		seed, replacements, retirements, lookup, region, waiting, 64
	), "same-key GPU generation replacement was rejected as retirement-only");
	check(region.replacements == replacements && region.retirements == retirements &&
		waiting.empty(), "same-key GPU generation swap changed its atomic cohort");
	std::cout << "GPU_SAME_KEY_GENERATION_SWAP_PASS replacements=1 retirements=1\n";
}

wt::WtChunkKey read_key() {
	wt::WtChunkKey key; int lod;
	check(bool(std::cin >> key.x >> key.y >> key.z >> lod) && lod >= 0 && lod <= wt::kWtMaximumLod,
		"invalid replay key");
	key.lod = static_cast<std::uint8_t>(lod);
	return key;
}

Keys read_keys() {
	std::size_t count; check(bool(std::cin >> count) && count <= 16384, "invalid replay count");
	Keys keys; for (std::size_t i = 0; i < count; ++i) keys.push_back(read_key());
	normalize(keys); return keys;
}

void print_keys(const Keys &keys) {
	std::cout << '[';
	for (std::size_t i = 0; i < keys.size(); ++i) {
		const auto &k = keys[i];
		if (i) std::cout << ',';
		std::cout << '[' << k.x << ',' << k.y << ',' << k.z << ',' << int(k.lod) << ']';
	}
	std::cout << ']';
}

void replay(int iterations) {
	const auto seed = read_key();
	const auto replacements = read_keys(), retirements = read_keys();
	std::size_t count; check(bool(std::cin >> count) && count <= 16384, "invalid boundary count");
	std::map<wt::WtChunkKey, wt::WtGpuPublicationBoundary> boundaries;
	for (std::size_t i = 0; i < count; ++i) {
		const auto key = read_key(); int mask, compatible;
		check(bool(std::cin >> mask >> compatible) && mask >= 0 && mask < 64,
			"invalid boundary mask");
		boundaries[key] = {static_cast<std::uint8_t>(mask), compatible != 0};
	}
	const auto lookup = [&](const auto &key, auto &boundary) {
		const auto found = boundaries.find(key);
		if (found == boundaries.end()) return false;
		boundary = found->second; return true;
	};
	std::vector<double> times;
	wt::WtPublicationDependencyGraph dependencies;
	wt::WtChunkPublicationRegion region; Keys waiting;
	bool built = false;
	for (int i = 0; i <= iterations; ++i) {
		const auto start = std::chrono::steady_clock::now();
		built = wt::wt_build_gpu_chunk_publication_cohort(seed, replacements, retirements, lookup, region, waiting, 4096, &dependencies);
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
		if (i) times.push_back(us);
	}
	std::sort(times.begin(), times.end());
	// Compare the exact captured region under the G23 clipped world hierarchy.
	const Authority authority = [](const auto &key) {
		const auto b = wt::wt_chunk_bounds(key);
		return b.minimum.x < 2048 && b.maximum.x > 0 && b.minimum.z < 2048 && b.maximum.z > 0 &&
			b.minimum.y < 128 && b.maximum.y > -128;
	};
	std::vector<double> coverage_times, reference_times;
	bool covered = false, expected = false;
	for (int i = 0; i <= iterations; ++i) {
		const auto start = std::chrono::steady_clock::now();
		expected = reference_coverage(region, authority);
		const auto middle = std::chrono::steady_clock::now();
		covered = wt::wt_chunk_publication_region_has_complete_authoritative_coverage(region, authority);
		const auto end = std::chrono::steady_clock::now();
		check(covered == expected, "captured region coverage differs from oracle");
		if (i) {
			reference_times.push_back(std::chrono::duration<double, std::micro>(middle - start).count());
			coverage_times.push_back(std::chrono::duration<double, std::micro>(end - middle).count());
		}
	}
	std::sort(coverage_times.begin(), coverage_times.end());
	std::sort(reference_times.begin(), reference_times.end());
	std::cout << "{\"built\":" << (built ? "true" : "false")
		<< ",\"coverage_matches_oracle\":true,\"covered\":" << (covered ? "true" : "false")
		<< ",\"coverage_median_us\":" << coverage_times[coverage_times.size() / 2]
		<< ",\"reference_coverage_median_us\":" << reference_times[reference_times.size() / 2]
		<< ",\"iterations\":" << iterations << ",\"median_us\":" << times[times.size() / 2]
		<< ",\"p95_us\":" << times[(times.size() - 1) * 95 / 100]
		<< ",\"selected\":"; print_keys(region.replacements);
	std::cout << ",\"retirements\":"; print_keys(region.retirements);
	std::cout << ",\"waiting_masks\":"; print_keys(waiting);
	std::cout << "}\n";
}
} // namespace

int main(int argc, char **argv) {
	if (argc == 3 && std::string(argv[1]) == "--replay") {
		const int iterations = std::stoi(argv[2]);
		check(iterations > 0 && iterations <= 1000, "invalid iterations");
		replay(iterations);
	} else {
		regression();
		spatial_dependency_regression();
		dependency_snapshot_regression();
		sparse_fine_face_cohort_regression();
		cold_same_lod_cohort_regression();
		same_key_replacement_retirement_regression();
		coverage_regression();
	}
}
