#include "backend/wt_transvoxel_mit_backend.h"
#include "meshing/wt_chunk_mesher.h"
#include "storage/wt_hash256.h"
#include "storage/wt_chunk_page_sample_source.h"
#include "storage/wt_procedural_world_source.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

namespace wt = world_transvoxel;

namespace {

class ProceduralSource final : public wt::WtChunkSampleSource {
public:
	explicit ProceduralSource(const wt::WtProceduralWorldDescriptor &descriptor)
		: descriptor_(descriptor) {}

	bool sample(const wt::WtGridPoint &point, wt::WtScalarSample &output) const noexcept override {
		return wt::wt_sample_procedural_world(descriptor_, point, output);
	}

private:
	const wt::WtProceduralWorldDescriptor &descriptor_;
};

void u32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
	for (unsigned shift = 0; shift < 32; shift += 8) {
		bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

void u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
	for (unsigned shift = 0; shift < 64; shift += 8) {
		bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

void f32(std::vector<std::uint8_t> &bytes, float value) {
	std::uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&bits, &value, sizeof(bits));
	u32(bytes, bits);
}

void vertex(std::vector<std::uint8_t> &bytes, const wt::WtCellVertex &value) {
	f32(bytes, value.position.x);
	f32(bytes, value.position.y);
	f32(bytes, value.position.z);
	f32(bytes, value.normal.x);
	f32(bytes, value.normal.y);
	f32(bytes, value.normal.z);
	bytes.push_back(static_cast<std::uint8_t>(value.material));
	bytes.push_back(static_cast<std::uint8_t>(value.material >> 8));
	bytes.push_back(value.material_authored ? 1U : 0U);
}

bool write_bytes(std::ofstream &output, const std::vector<std::uint8_t> &bytes) {
	output.write(reinterpret_cast<const char *>(bytes.data()),
		static_cast<std::streamsize>(bytes.size()));
	return output.good();
}

std::vector<std::uint8_t> mesh_payload(const wt::WtChunkMeshResult &mesh) {
	std::vector<std::uint8_t> payload;
	payload.reserve(mesh.regular.vertices.size() * 27 + mesh.regular.indices.size() * 4);
	for (const auto &item : mesh.regular.vertices) vertex(payload, item);
	for (const std::uint32_t item : mesh.regular.indices) u32(payload, item);
	return payload;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 3 || argc > 4 ||
		(argc == 4 && std::string(argv[3]) != "--page-parity")) {
		std::fprintf(stderr, "usage: wt_base_coverage_atlas_probe OUTPUT ROOT_COUNT [--page-parity]\n");
		return 2;
	}
	const bool page_parity = argc == 4;
	char *end = nullptr;
	const unsigned long requested = std::strtoul(argv[2], &end, 10);
	if (end == argv[2] || *end != '\0' || requested == 0 || requested > 512) {
		std::fprintf(stderr, "ROOT_COUNT must be 1..512\n");
		return 2;
	}
	wt::WtProceduralWorldDescriptor descriptor;
	descriptor.chunk_count_x = 128;
	descriptor.chunk_count_y = 16;
	descriptor.chunk_count_z = 128;
	descriptor.chunk_y = -8;
	descriptor.source_revision = 190327;
	descriptor.seed = 19023;
	descriptor.mode = wt::WtProceduralWorldMode::FourBiomesLakesCavesRoads;
	descriptor.bottom_boundary_policy = wt::WtProceduralBottomBoundaryPolicy::Bedrock;
	descriptor.bottom_boundary_thickness_cells = 16;
	std::vector<wt::WtChunkKey> keys;
	if (!wt::wt_append_procedural_lod_keys(descriptor, 3, keys, 512) ||
		keys.size() != 512) {
		std::fprintf(stderr, "g23 LOD3 root inventory is not 512\n");
		return 3;
	}
	// Include all vertical strata in each prefix; a horizontal-only prefix would
	// badly bias both mesh-size and empty-record measurements.
	std::sort(keys.begin(), keys.end(), [](const auto &left, const auto &right) {
		return std::tuple(left.x, left.z, left.y) < std::tuple(right.x, right.z, right.y);
	});
	keys.resize(requested);
	std::filesystem::create_directories(std::filesystem::path(argv[1]).parent_path());
	std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
	if (!output) return 4;
	std::vector<std::uint8_t> header {'W', 'T', 'B', 'A'};
	u32(header, 1); // probe format version
	u64(header, descriptor.source_revision);
	u32(header, descriptor.seed);
	u32(header, static_cast<std::uint32_t>(descriptor.mode));
	u32(header, descriptor.chunk_count_x);
	u32(header, descriptor.chunk_count_y);
	u32(header, descriptor.chunk_count_z);
	u32(header, static_cast<std::uint32_t>(descriptor.chunk_y));
	u32(header, static_cast<std::uint32_t>(descriptor.bottom_boundary_policy));
	u32(header, descriptor.bottom_boundary_thickness_cells);
	u32(header, 3); // LOD
	u32(header, static_cast<std::uint32_t>(keys.size()));
	if (!write_bytes(output, header)) return 4;
	const auto started = std::chrono::steady_clock::now();
	wt::WtTransvoxelMitBackend backend;
	wt::WtChunkMesher mesher(backend);
	ProceduralSource source(descriptor);
	wt::WtChunkMeshingScratch scratch;
	wt::WtChunkMeshResult mesh;
	std::uint64_t vertex_count = 0;
	std::uint64_t index_count = 0;
	std::uint32_t empty_count = 0;
	for (const wt::WtChunkKey &key : keys) {
		wt::WtChunkMeshingInput input;
		input.key = key;
		input.transition_mask = 0;
		input.cached_transition_mask = 0;
		if (mesher.mesh(input, source, mesh, scratch) != wt::WtChunkMeshingStatus::Ok) {
			std::fprintf(stderr, "meshing failed at (%d,%d,%d)\n", key.x, key.y, key.z);
			return 5;
		}
		const std::vector<std::uint8_t> payload = mesh_payload(mesh);
		if (page_parity) {
			std::uint64_t bytes_read = 0;
			const auto page_completion = wt::wt_generate_procedural_page(
				descriptor, key, wt::WtGenerationToken {1}, bytes_read
			);
			wt::WtChunkPageView view;
			wt::WtChunkPage page;
			if (page_completion.status != wt::WtPageLoadStatus::Ok ||
				!page_completion.page_bytes ||
				wt::wt_open_chunk_page({
					page_completion.page_bytes->data(), page_completion.page_bytes->size()
				}, view) != wt::WtChunkPageStatus::Ok ||
				wt::wt_decode_chunk_page(view, page) != wt::WtChunkPageStatus::Ok) {
				std::fprintf(stderr, "page decode failed at (%d,%d,%d)\n", key.x, key.y, key.z);
				return 6;
			}
			wt::WtChunkPageSampleSource page_source(page);
			wt::WtChunkMeshResult page_mesh;
			wt::WtChunkMeshingScratch page_scratch;
			if (mesher.mesh(input, page_source, page_mesh, page_scratch) !=
				wt::WtChunkMeshingStatus::Ok || mesh_payload(page_mesh) != payload) {
				std::fprintf(stderr, "page-backed mesh parity failed at (%d,%d,%d)\n", key.x, key.y, key.z);
				return 7;
			}
		}
		const auto digest = wt::wt_sha256(payload.data(), payload.size());
		std::vector<std::uint8_t> record;
		u32(record, static_cast<std::uint32_t>(key.x));
		u32(record, static_cast<std::uint32_t>(key.y));
		u32(record, static_cast<std::uint32_t>(key.z));
		u32(record, mesh.regular.indices.empty() ? 1U : 0U);
		u32(record, static_cast<std::uint32_t>(mesh.regular.vertices.size()));
		u32(record, static_cast<std::uint32_t>(mesh.regular.indices.size()));
		u32(record, static_cast<std::uint32_t>(payload.size()));
		record.insert(record.end(), digest.begin(), digest.end());
		if (!write_bytes(output, record) || !write_bytes(output, payload)) return 4;
		vertex_count += mesh.regular.vertices.size();
		index_count += mesh.regular.indices.size();
		empty_count += mesh.regular.indices.empty() ? 1U : 0U;
	}
	output.close();
	const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - started).count();
	std::printf("WT_BASE_ATLAS_PROBE_PASS roots=%zu empty=%u vertices=%llu indices=%llu bytes=%llu bake_ms=%lld page_parity=%d\n",
		keys.size(), empty_count,
		static_cast<unsigned long long>(vertex_count),
		static_cast<unsigned long long>(index_count),
		static_cast<unsigned long long>(std::filesystem::file_size(argv[1])),
		static_cast<long long>(elapsed_ms), page_parity ? 1 : 0);
	return 0;
}
