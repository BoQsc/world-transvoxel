#include "backend/wt_transvoxel_mit_backend.h"
#include "bake/wt_chunk_baker.h"
#include "meshing/wt_chunk_mesher.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_chunk_page.h"
#include "storage/wt_chunk_page_sample_source.h"
#include "storage/wt_hash256.h"
#include "storage/wt_world_manifest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace wt = world_transvoxel;

namespace {

constexpr std::size_t kPageCount = 17;
constexpr std::size_t kPageMeshChunkCount = 4;
constexpr std::size_t kTransitionChunkCount = 1;
constexpr std::uint64_t kSourceRevision = 5001;
constexpr wt::WtChunkKey kTransitionKey = { 0, 0, 0, 1 };
constexpr std::uint8_t kTransitionMask = 0x2aU;
constexpr std::array<wt::WtChunkKey, kPageMeshChunkCount>
	kPageMeshKeys = {{
		{ 0, 0, 0, 0 },
		{ 1, 0, 0, 0 },
		{ 0, 0, 1, 0 },
		{ 1, 0, 1, 0 },
	}};

struct MeshSummary {
	std::uint64_t checksum = 14695981039346656037ULL;
	std::uint64_t vertices = 0;
	std::uint64_t indices = 0;

	bool operator==(const MeshSummary &other) const noexcept {
		return checksum == other.checksum &&
			vertices == other.vertices &&
			indices == other.indices;
	}
};

void mix(MeshSummary &summary, std::uint64_t value) noexcept {
	summary.checksum ^= value;
	summary.checksum *= 1099511628211ULL;
}

void add_buffer(
	MeshSummary &summary,
	const wt::WtChunkMeshBuffer &buffer
) noexcept {
	summary.vertices += buffer.vertices.size();
	summary.indices += buffer.indices.size();
	mix(summary, buffer.vertices.size());
	mix(summary, buffer.indices.size());
	if (!buffer.indices.empty()) {
		mix(summary, buffer.indices.front());
		mix(summary, buffer.indices.back());
	}
}

void add_result(
	MeshSummary &summary,
	const wt::WtChunkMeshResult &result
) noexcept {
	mix(summary, static_cast<std::uint32_t>(result.key.x));
	mix(summary, static_cast<std::uint32_t>(result.key.y));
	mix(summary, static_cast<std::uint32_t>(result.key.z));
	mix(summary, result.key.lod);
	mix(summary, result.transition_mask);
	add_buffer(summary, result.regular);
	for (const wt::WtChunkMeshBuffer &transition : result.transitions) {
		add_buffer(summary, transition);
	}
}

wt::WtHash256 hash_text(const char *text) {
	const std::string value(text);
	return wt::wt_sha256(
		reinterpret_cast<const std::uint8_t *>(value.data()),
		value.size()
	);
}

wt::WtDependencyEntry dependency(
	wt::WtDependencyKind kind,
	const char *label,
	const char *version,
	const char *identity
) {
	return { kind, label, version, hash_text(identity) };
}

bool write_file(
	const std::filesystem::path &path,
	const std::vector<std::uint8_t> &bytes
) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) {
		return false;
	}
	if (!bytes.empty()) {
		output.write(
			reinterpret_cast<const char *>(bytes.data()),
			static_cast<std::streamsize>(bytes.size())
		);
	}
	return static_cast<bool>(output);
}

class SphereSource final : public wt::WtChunkSampleSource {
public:
	wt::WtGridPoint center = { 16, 16, 16 };
	double radius = 18.0;

	bool sample(
		const wt::WtGridPoint &point,
		wt::WtScalarSample &output
	) const noexcept override {
		const double x = static_cast<double>(point.x - center.x);
		const double y = static_cast<double>(point.y - center.y);
		const double z = static_cast<double>(point.z - center.z);
		output.density = static_cast<float>(
			std::sqrt(x * x + y * y + z * z) - radius
		);
		output.material = 3;
		return true;
	}
};

bool page_mesh_key(const wt::WtChunkKey &key) noexcept {
	return std::find(
		kPageMeshKeys.begin(),
		kPageMeshKeys.end(),
		key
	) != kPageMeshKeys.end();
}

bool append_transition_support_keys(
	std::vector<wt::WtChunkKey> &keys
) {
	for (wt::WtChunkFace face : {
		wt::WtChunkFace::PositiveX,
		wt::WtChunkFace::PositiveY,
		wt::WtChunkFace::PositiveZ,
	}) {
		std::array<wt::WtChunkKey, 4> support{};
		if (!wt::wt_transition_support_page_keys(
				kTransitionKey,
				face,
				support
			)) {
			return false;
		}
		keys.insert(keys.end(), support.begin(), support.end());
	}
	return true;
}

struct Fixture {
	std::filesystem::path root;
	std::filesystem::path world_path;
	std::vector<wt::WtBakedChunkPage> pages;
	std::size_t bytes_per_batch = 0;

	Fixture() = default;
	Fixture(const Fixture &) = delete;
	Fixture &operator=(const Fixture &) = delete;
	Fixture(Fixture &&other) noexcept :
			root(std::move(other.root)),
			world_path(std::move(other.world_path)),
			pages(std::move(other.pages)),
			bytes_per_batch(other.bytes_per_batch) {
		other.root.clear();
	}

	~Fixture() {
		if (root.empty()) {
			return;
		}
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}
};

bool make_fixture(Fixture &fixture) {
	fixture.root = std::filesystem::temp_directory_path() /
		("wt_m5_pipeline_budget_" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()
		));
	std::error_code error;
	if (!std::filesystem::create_directories(fixture.root, error) || error) {
		return false;
	}
	std::vector<wt::WtChunkKey> keys(
		kPageMeshKeys.begin(),
		kPageMeshKeys.end()
	);
	keys.push_back(kTransitionKey);
	if (!append_transition_support_keys(keys) ||
		keys.size() != kPageCount) {
		return false;
	}
	const SphereSource source;
	wt::WtChunkBaker baker(keys.size());
	if (baker.bake(keys, kSourceRevision, source, fixture.pages) !=
			wt::WtChunkBakeStatus::Ok ||
		fixture.pages.size() != kPageCount) {
		return false;
	}

	wt::WtWorldManifest manifest;
	manifest.source_revision = kSourceRevision;
	manifest.world_revision = 1;
	manifest.configuration_hash = hash_text("m5-pipeline-budget-configuration");
	manifest.dependencies = {
		dependency(wt::WtDependencyKind::SourceAsset,
			"pipeline-fixture", "", "pipeline-fixture-source"),
		dependency(wt::WtDependencyKind::Generator,
			"world-transvoxel-baker", "0.5.0-m5", "pipeline-generator"),
		dependency(wt::WtDependencyKind::Configuration,
			"m5-pipeline-budget", "1", "m5-pipeline-budget-configuration"),
		dependency(wt::WtDependencyKind::Backend,
			"transvoxel-mit", "pinned", "pipeline-backend"),
		dependency(wt::WtDependencyKind::Godot,
			"godot", "4.6.3", "pipeline-godot"),
		dependency(wt::WtDependencyKind::GodotCpp,
			"godot-cpp", "e83fd090", "pipeline-godot-cpp"),
		dependency(wt::WtDependencyKind::Toolchain,
			"zig", "0.16.0", "pipeline-zig"),
	};
	for (const wt::WtBakedChunkPage &page : fixture.pages) {
		manifest.pages.push_back({
			page.key,
			page.bytes.size(),
			page.content_hash,
		});
		fixture.bytes_per_batch += page.bytes.size();
		if (!write_file(
				wt::wt_page_object_path(fixture.root, page.content_hash),
				page.bytes
			)) {
			return false;
		}
	}
	std::vector<std::uint8_t> world_bytes;
	if (wt::wt_write_world_manifest(manifest, world_bytes) !=
			wt::WtWorldManifestStatus::Ok) {
		return false;
	}
	fixture.world_path = fixture.root / "world.wtworld";
	return write_file(fixture.world_path, world_bytes);
}

std::size_t page_index(
	const Fixture &fixture,
	const wt::WtChunkKey &key
) noexcept {
	for (std::size_t index = 0; index < fixture.pages.size(); ++index) {
		if (fixture.pages[index].key == key) {
			return index;
		}
	}
	return fixture.pages.size();
}

bool load_decode_batch(
	wt::WtAsyncStorageService &service,
	const Fixture &fixture,
	std::uint64_t generation,
	std::vector<wt::WtChunkPage> &decoded
) {
	decoded.clear();
	decoded.resize(fixture.pages.size());
	for (std::size_t index = 0; index < fixture.pages.size(); ++index) {
		if (service.request_page(
				fixture.pages[index].key,
				{ generation },
				static_cast<std::int32_t>(fixture.pages.size() - index)
			) != wt::WtAsyncStorageStatus::Ok) {
			return false;
		}
	}
	for (std::size_t completed = 0; completed < fixture.pages.size(); ++completed) {
		wt::WtPageLoadCompletion completion;
		if (!service.wait_pop_completion(
				completion,
				std::chrono::seconds(5)
			) ||
			completion.status != wt::WtPageLoadStatus::Ok ||
			!completion.page_bytes) {
			return false;
		}
		const std::size_t index = page_index(fixture, completion.key);
		if (index == fixture.pages.size()) {
			return false;
		}
		wt::WtChunkPageView view;
		const std::vector<std::uint8_t> &bytes = *completion.page_bytes;
		if (wt::wt_open_chunk_page(
				{ bytes.data(), bytes.size() },
				view
			) != wt::WtChunkPageStatus::Ok ||
			wt::wt_decode_chunk_page(view, decoded[index]) !=
				wt::WtChunkPageStatus::Ok) {
			return false;
		}
	}
	return true;
}

bool mesh_decoded_pages(
	const std::vector<wt::WtChunkPage> &pages,
	const wt::WtChunkMesher &mesher,
	wt::WtChunkMeshingScratch &scratch,
	wt::WtChunkMeshResult &output,
	MeshSummary &summary
) {
	summary = {};
	for (const wt::WtChunkPage &page : pages) {
		if (!page_mesh_key(page.metadata.key)) {
			continue;
		}
		const wt::WtChunkPageSampleSource source(page);
		if (mesher.mesh(
				{ page.metadata.key, 0, 0.0F, 0.25F },
				source,
				output,
				scratch
			) != wt::WtChunkMeshingStatus::Ok ||
			output.regular.indices.empty()) {
			return false;
		}
		add_result(summary, output);
	}
	return true;
}

bool mesh_transition_chunks(
	const std::vector<wt::WtChunkPage> &pages,
	const wt::WtChunkMesher &mesher,
	wt::WtChunkMeshingScratch &scratch,
	wt::WtChunkMeshResult &output,
	MeshSummary &summary
) {
	summary = {};
	const wt::WtChunkPage *coarse_page = nullptr;
	for (const wt::WtChunkPage &page : pages) {
		if (page.metadata.key == kTransitionKey) {
			coarse_page = &page;
			break;
		}
	}
	if (coarse_page == nullptr) {
		return false;
	}
	wt::WtChunkPageSampleSource source(*coarse_page);
	for (const wt::WtChunkPage &page : pages) {
		if (page.metadata.key.lod == 0 &&
			!page_mesh_key(page.metadata.key) &&
			source.add_transition_support_page(page) !=
				wt::WtChunkPageSampleSourceStatus::Ok) {
			return false;
		}
	}
	if (!source.has_transition_support(kTransitionMask) ||
		mesher.mesh(
			{ kTransitionKey, kTransitionMask, 0.0F, 0.25F },
			source,
			output,
			scratch
		) != wt::WtChunkMeshingStatus::Ok ||
		output.regular.indices.empty()) {
		return false;
	}
	for (std::size_t face = 0; face < output.transitions.size(); ++face) {
		const bool expected = (kTransitionMask & (1U << face)) != 0;
		if (expected && output.transitions[face].indices.empty()) {
			return false;
		}
	}
	add_result(summary, output);
	return true;
}

bool parse_count(
	const char *text,
	std::size_t &output
) noexcept {
	char *end = nullptr;
	const unsigned long long value = std::strtoull(text, &end, 10);
	if (end == text || *end != '\0' ||
		value == 0 ||
		value > std::numeric_limits<std::size_t>::max()) {
		return false;
	}
	output = static_cast<std::size_t>(value);
	return true;
}

} // namespace

int main(int argc, char **argv) {
	std::size_t measured_runs = 0;
	std::size_t warmup_runs = 0;
	for (int index = 1; index < argc; ++index) {
		if (std::string(argv[index]) == "--benchmark-runs" &&
			index + 1 < argc) {
			if (!parse_count(argv[++index], measured_runs)) {
				std::fprintf(stderr, "invalid --benchmark-runs\n");
				return 2;
			}
		} else if (std::string(argv[index]) == "--warmup-runs" &&
			index + 1 < argc) {
			if (!parse_count(argv[++index], warmup_runs)) {
				std::fprintf(stderr, "invalid --warmup-runs\n");
				return 2;
			}
		} else {
			std::fprintf(stderr, "unknown benchmark argument\n");
			return 2;
		}
	}
	if (measured_runs == 0 || warmup_runs == 0) {
		std::fprintf(stderr,
			"usage: --benchmark-runs N --warmup-runs N\n");
		return 2;
	}

	Fixture fixture;
	if (!make_fixture(fixture)) {
		std::fprintf(stderr, "M5_PIPELINE_BENCHMARK_FAIL fixture\n");
		return 1;
	}
	wt::WtAsyncStorageService storage({
		kPageCount,
		kPageCount,
		wt::kWtMaximumContainerSize,
	});
	if (storage.open(fixture.world_path, fixture.root) !=
			wt::WtAsyncStorageStatus::Ok) {
		std::fprintf(stderr, "M5_PIPELINE_BENCHMARK_FAIL storage_open\n");
		return 1;
	}
	const wt::WtMeshingBackend &backend = wt::wt_get_transvoxel_mit_backend();
	const wt::WtMeshingBackendInfo &backend_info = backend.get_info();
	if (!backend.is_available() ||
		std::string(backend_info.id) != "transvoxel_mit_official" ||
		std::string(backend_info.license) != "MIT" ||
		!backend_info.official_reference) {
		std::fprintf(stderr, "M5_PIPELINE_BENCHMARK_FAIL backend\n");
		return 1;
	}
	const wt::WtChunkMesher mesher(backend);
	wt::WtChunkMeshingScratch scratch;
	wt::WtChunkMeshResult mesh_output;
	std::vector<wt::WtChunkPage> decoded;
	MeshSummary expected_page_mesh;
	MeshSummary expected_transition_mesh;
	bool expected_ready = false;
	std::uint64_t generation = 1;
	const std::size_t total_runs = warmup_runs + measured_runs;

	for (std::size_t run = 0; run < total_runs; ++run) {
		const auto io_start = std::chrono::steady_clock::now();
		if (!load_decode_batch(storage, fixture, generation++, decoded)) {
			std::fprintf(stderr, "M5_PIPELINE_BENCHMARK_FAIL io_decode\n");
			return 1;
		}
		const auto io_end = std::chrono::steady_clock::now();

		MeshSummary page_mesh;
		const auto page_mesh_start = std::chrono::steady_clock::now();
		if (!mesh_decoded_pages(
				decoded,
				mesher,
				scratch,
				mesh_output,
				page_mesh
			)) {
			std::fprintf(stderr, "M5_PIPELINE_BENCHMARK_FAIL page_mesh\n");
			return 1;
		}
		const auto page_mesh_end = std::chrono::steady_clock::now();

		MeshSummary transition_mesh;
		const auto transition_mesh_start = std::chrono::steady_clock::now();
		if (!mesh_transition_chunks(
				decoded,
				mesher,
				scratch,
				mesh_output,
				transition_mesh
			)) {
			std::fprintf(stderr, "M5_PIPELINE_BENCHMARK_FAIL transition_mesh\n");
			return 1;
		}
		const auto transition_mesh_end = std::chrono::steady_clock::now();

		if (!expected_ready) {
			expected_page_mesh = page_mesh;
			expected_transition_mesh = transition_mesh;
			expected_ready = true;
		} else if (!(page_mesh == expected_page_mesh) ||
			!(transition_mesh == expected_transition_mesh)) {
			std::fprintf(stderr, "M5_PIPELINE_BENCHMARK_FAIL nondeterministic\n");
			return 1;
		}
		if (run >= warmup_runs) {
			const std::size_t measured_index = run - warmup_runs + 1;
			const auto io_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
				io_end - io_start
			).count();
			const auto page_mesh_ns =
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					page_mesh_end - page_mesh_start
				).count();
			const auto transition_mesh_ns =
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					transition_mesh_end - transition_mesh_start
				).count();
			std::printf(
				"M5_PIPELINE_BENCHMARK_SAMPLE run=%zu "
				"io_decode_ns=%lld page_mesh_ns=%lld transition_mesh_ns=%lld\n",
				measured_index,
				static_cast<long long>(io_ns),
				static_cast<long long>(page_mesh_ns),
				static_cast<long long>(transition_mesh_ns)
			);
		}
	}
	const wt::WtAsyncStorageMetrics metrics = storage.get_metrics();
	if (metrics.successful_pages != total_runs * kPageCount ||
		metrics.failed_pages != 0 ||
		metrics.bytes_read != total_runs * fixture.bytes_per_batch) {
		std::fprintf(stderr, "M5_PIPELINE_BENCHMARK_FAIL storage_metrics\n");
		return 1;
	}
	std::printf(
		"M5_PIPELINE_BENCHMARK_PASS runs=%zu warmup=%zu pages_per_run=%zu "
		"page_mesh_chunks_per_run=%zu transition_chunks_per_run=%zu "
		"bytes_per_run=%zu page_vertices=%llu page_triangles=%llu "
		"transition_vertices=%llu transition_triangles=%llu backend=%s "
		"transition_source=decoded_pages\n",
		measured_runs,
		warmup_runs,
		kPageCount,
		kPageMeshChunkCount,
		kTransitionChunkCount,
		fixture.bytes_per_batch,
		static_cast<unsigned long long>(expected_page_mesh.vertices),
		static_cast<unsigned long long>(expected_page_mesh.indices / 3),
		static_cast<unsigned long long>(expected_transition_mesh.vertices),
		static_cast<unsigned long long>(expected_transition_mesh.indices / 3),
		backend_info.id
	);
	return 0;
}
