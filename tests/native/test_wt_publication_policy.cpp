#include "services/wt_chunk_publication_policy.h"

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
	wt::WtChunkPublicationRegion region; Keys waiting;
	bool built = false;
	for (int i = 0; i <= iterations; ++i) {
		const auto start = std::chrono::steady_clock::now();
		built = wt::wt_build_gpu_chunk_publication_cohort(seed, replacements, retirements, lookup, region, waiting);
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
		coverage_regression();
	}
}
