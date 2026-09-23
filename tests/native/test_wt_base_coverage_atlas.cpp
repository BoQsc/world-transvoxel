#include "render/wt_base_coverage_atlas.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace wt = world_transvoxel;

namespace {

wt::WtProceduralWorldDescriptor g23() {
	wt::WtProceduralWorldDescriptor source;
	source.chunk_count_x = 128;
	source.chunk_count_y = 16;
	source.chunk_count_z = 128;
	source.chunk_y = -8;
	source.source_revision = 190327;
	source.seed = 19023;
	source.mode = wt::WtProceduralWorldMode::FourBiomesLakesCavesRoads;
	source.bottom_boundary_policy = wt::WtProceduralBottomBoundaryPolicy::Bedrock;
	source.bottom_boundary_thickness_cells = 16;
	return source;
}

std::vector<wt::WtChunkKey> inventory() {
	std::vector<wt::WtChunkKey> keys;
	for (int x = 0; x < 16; ++x) {
		for (int z = 0; z < 16; ++z) {
			for (int y = -1; y <= 0; ++y) keys.push_back({x, y, z, 3});
		}
	}
	return keys;
}

bool expect(
	const std::vector<std::uint8_t> &bytes,
	const wt::WtProceduralWorldDescriptor &source,
	const std::vector<wt::WtChunkKey> &keys,
	wt::WtBaseCoverageAtlasStatus wanted
) {
	wt::WtBaseCoverageAtlasView view;
	const auto status = wt::wt_open_base_coverage_atlas(
		{bytes.data(), bytes.size()}, source, 3, keys, view
	);
	if (status != wanted || (status != wt::WtBaseCoverageAtlasStatus::Ok &&
			!view.roots.empty())) {
		std::fprintf(stderr, "atlas status %u, expected %u, roots %zu\n",
			static_cast<unsigned>(status), static_cast<unsigned>(wanted),
			view.roots.size());
		return false;
	}
	if (status == wt::WtBaseCoverageAtlasStatus::Ok) {
		std::size_t empties = 0, vertices = 0, indices = 0;
		for (const auto &root : view.roots) {
			empties += root.proven_empty;
			vertices += root.vertex_count;
			indices += root.index_count;
		}
		if (view.roots.size() != 512 || empties != 246 ||
			vertices != 86049 || indices != 460344) return false;
	}
	return true;
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 2) return 2;
	std::ifstream input(argv[1], std::ios::binary);
	if (!input) return 3;
	const std::vector<std::uint8_t> valid {
		std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()
	};
	auto source = g23();
	const auto keys = inventory();
	if (!expect(valid, source, keys, wt::WtBaseCoverageAtlasStatus::Ok)) return 4;
	source.world_revision = 1;
	if (!expect(valid, source, keys, wt::WtBaseCoverageAtlasStatus::InvalidInput)) return 5;
	source = g23();
	source.seed += 1;
	if (!expect(valid, source, keys, wt::WtBaseCoverageAtlasStatus::SourceMismatch)) return 6;
	source = g23();
	auto partial = keys;
	partial.pop_back();
	if (!expect(valid, source, partial,
			wt::WtBaseCoverageAtlasStatus::IncompleteInventory)) return 7;
	auto corrupt = valid;
	corrupt.back() ^= 1;
	if (!expect(corrupt, source, keys, wt::WtBaseCoverageAtlasStatus::HashMismatch)) return 8;
	corrupt = valid;
	corrupt.resize(corrupt.size() - 1);
	if (!expect(corrupt, source, keys, wt::WtBaseCoverageAtlasStatus::Truncated)) return 9;
	corrupt = valid;
	corrupt[56 + 12] = 2; // first record's empty flag
	if (!expect(corrupt, source, keys, wt::WtBaseCoverageAtlasStatus::InvalidRecord)) return 10;
	std::puts("WT_BASE_COVERAGE_ATLAS_RUNTIME_PASS roots=512 empty=246");
	return 0;
}
