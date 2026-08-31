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
	std::cout << "{\"built\":" << (built ? "true" : "false")
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
	}
}
