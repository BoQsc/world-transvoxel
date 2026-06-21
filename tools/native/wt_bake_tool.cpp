#include "bake/wt_chunk_baker.h"
#include "bake/wt_dense_grid_source.h"
#include "storage/wt_binary_io.h"
#include "storage/wt_world_manifest.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace wt = world_transvoxel;

namespace {

constexpr std::uint64_t kMaximumInputFileSize = 1024ULL * 1024ULL * 1024ULL;

bool read_file(
	const std::filesystem::path &path,
	std::vector<std::uint8_t> &output
) {
	output.clear();
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input) return false;
	const std::streamoff size = input.tellg();
	if (size < 0 ||
		static_cast<std::uint64_t>(size) > kMaximumInputFileSize) {
		return false;
	}
	output.resize(static_cast<std::size_t>(size));
	input.seekg(0);
	if (!output.empty()) {
		input.read(
			reinterpret_cast<char *>(output.data()),
			static_cast<std::streamsize>(output.size())
		);
	}
	return input.good() || input.eof();
}

bool parse_i64(const char *text, std::int64_t &output) {
	errno = 0;
	char *end = nullptr;
	const long long value = std::strtoll(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0') return false;
	output = static_cast<std::int64_t>(value);
	return true;
}

bool parse_u64(const char *text, std::uint64_t &output) {
	errno = 0;
	char *end = nullptr;
	const unsigned long long value = std::strtoull(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || text[0] == '-') {
		return false;
	}
	output = static_cast<std::uint64_t>(value);
	return true;
}

std::uint8_t hex_nibble(char value) {
	if (value >= '0' && value <= '9') {
		return static_cast<std::uint8_t>(value - '0');
	}
	if (value >= 'a' && value <= 'f') {
		return static_cast<std::uint8_t>(value - 'a' + 10);
	}
	if (value >= 'A' && value <= 'F') {
		return static_cast<std::uint8_t>(value - 'A' + 10);
	}
	return 0xffU;
}

bool parse_hash(const char *text, wt::WtHash256 &output) {
	const std::string value(text);
	if (value.size() != output.size() * 2) return false;
	for (std::size_t index = 0; index < output.size(); ++index) {
		const std::uint8_t high = hex_nibble(value[index * 2]);
		const std::uint8_t low = hex_nibble(value[index * 2 + 1]);
		if (high > 15 || low > 15) return false;
		output[index] = static_cast<std::uint8_t>((high << 4) | low);
	}
	return !wt::wt_is_zero_hash(output);
}

std::string hash_hex(const wt::WtHash256 &hash) {
	static constexpr char digits[] = "0123456789abcdef";
	std::string output(hash.size() * 2, '0');
	for (std::size_t index = 0; index < hash.size(); ++index) {
		output[index * 2] = digits[hash[index] >> 4];
		output[index * 2 + 1] = digits[hash[index] & 0x0fU];
	}
	return output;
}

wt::WtHash256 hash_text(const std::string &value) {
	return wt::wt_sha256(
		reinterpret_cast<const std::uint8_t *>(value.data()),
		value.size()
	);
}

bool read_keys(
	const std::filesystem::path &path,
	std::vector<wt::WtChunkKey> &output
) {
	output.clear();
	std::ifstream input(path);
	if (!input) return false;
	long long x = 0;
	long long y = 0;
	long long z = 0;
	unsigned int lod = 0;
	while (input >> x >> y >> z >> lod) {
		if (x < std::numeric_limits<std::int32_t>::min() ||
			x > std::numeric_limits<std::int32_t>::max() ||
			y < std::numeric_limits<std::int32_t>::min() ||
			y > std::numeric_limits<std::int32_t>::max() ||
			z < std::numeric_limits<std::int32_t>::min() ||
			z > std::numeric_limits<std::int32_t>::max() ||
			lod > std::numeric_limits<std::uint8_t>::max()) {
			return false;
		}
		output.push_back({
			static_cast<std::int32_t>(x),
			static_cast<std::int32_t>(y),
			static_cast<std::int32_t>(z),
			static_cast<std::uint8_t>(lod),
		});
		if (output.size() > wt::kWtMaximumWorldPageCount) return false;
	}
	return input.eof() && !output.empty();
}

bool decode_densities(
	const std::vector<std::uint8_t> &bytes,
	std::size_t count,
	std::vector<float> &output
) {
	if (count > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
		bytes.size() != count * sizeof(float)) {
		return false;
	}
	wt::WtBinaryReader reader({ bytes.data(), bytes.size() });
	output.resize(count);
	for (float &value : output) {
		if (reader.read_f32(value) != wt::WtBinaryStatus::Ok) return false;
	}
	return reader.remaining() == 0;
}

bool decode_materials(
	const std::vector<std::uint8_t> &bytes,
	std::size_t count,
	std::vector<std::uint16_t> &output
) {
	if (count > std::numeric_limits<std::size_t>::max() /
			sizeof(std::uint16_t) ||
		bytes.size() != count * sizeof(std::uint16_t)) {
		return false;
	}
	wt::WtBinaryReader reader({ bytes.data(), bytes.size() });
	output.resize(count);
	for (std::uint16_t &value : output) {
		if (reader.read_u16(value) != wt::WtBinaryStatus::Ok) return false;
	}
	return reader.remaining() == 0;
}

bool write_file(
	const std::filesystem::path &path,
	const std::vector<std::uint8_t> &bytes
) {
	std::ofstream output(path, std::ios::binary);
	if (!output) return false;
	if (!bytes.empty()) {
		output.write(
			reinterpret_cast<const char *>(bytes.data()),
			static_cast<std::streamsize>(bytes.size())
		);
	}
	return static_cast<bool>(output);
}

int bake_dense(int argc, char **argv) {
	if (argc != 21) return 1;
	const std::filesystem::path density_path = argv[2];
	const std::string material_argument = argv[3];
	const std::filesystem::path key_path = argv[4];
	const std::filesystem::path output_path = argv[5];
	wt::WtDenseGridDescriptor descriptor;
	std::uint64_t dimensions[3]{};
	std::uint64_t default_material = 0;
	std::uint64_t source_revision = 0;
	if (!parse_i64(argv[6], descriptor.origin.x) ||
		!parse_i64(argv[7], descriptor.origin.y) ||
		!parse_i64(argv[8], descriptor.origin.z) ||
		!parse_u64(argv[9], dimensions[0]) ||
		!parse_u64(argv[10], dimensions[1]) ||
		!parse_u64(argv[11], dimensions[2]) ||
		!parse_u64(argv[12], descriptor.spacing) ||
		!parse_u64(argv[13], source_revision) ||
		!parse_u64(argv[14], default_material) ||
		dimensions[0] > std::numeric_limits<std::uint32_t>::max() ||
		dimensions[1] > std::numeric_limits<std::uint32_t>::max() ||
		dimensions[2] > std::numeric_limits<std::uint32_t>::max() ||
		default_material > std::numeric_limits<std::uint16_t>::max()) {
		return 2;
	}
	descriptor.dimension_x = static_cast<std::uint32_t>(dimensions[0]);
	descriptor.dimension_y = static_cast<std::uint32_t>(dimensions[1]);
	descriptor.dimension_z = static_cast<std::uint32_t>(dimensions[2]);
	descriptor.default_material =
		static_cast<std::uint16_t>(default_material);
	wt::WtHash256 configuration_hash{};
	wt::WtHash256 backend_hash{};
	if (!parse_hash(argv[15], configuration_hash) ||
		!parse_hash(argv[16], backend_hash)) {
		return 2;
	}
	const std::string generator_version = argv[17];
	const std::string godot_version = argv[18];
	const std::string godot_cpp_version = argv[19];
	const std::string toolchain_version = argv[20];
	if (generator_version.empty() || godot_version.empty() ||
		godot_cpp_version.empty() || toolchain_version.empty()) {
		return 2;
	}
	if (descriptor.dimension_x == 0 || descriptor.dimension_y == 0 ||
		descriptor.dimension_z == 0 ||
		descriptor.dimension_x >
			std::numeric_limits<std::size_t>::max() /
				descriptor.dimension_y) {
		return 2;
	}
	const std::size_t plane =
		static_cast<std::size_t>(descriptor.dimension_x) *
		descriptor.dimension_y;
	if (plane > std::numeric_limits<std::size_t>::max() /
			descriptor.dimension_z) {
		return 2;
	}
	const std::size_t sample_count = plane * descriptor.dimension_z;
	std::vector<std::uint8_t> density_bytes;
	std::vector<std::uint8_t> material_bytes;
	std::vector<float> densities;
	std::vector<std::uint16_t> materials;
	if (!read_file(density_path, density_bytes) ||
		!decode_densities(density_bytes, sample_count, densities) ||
		(material_argument != "-" &&
			(!read_file(material_argument, material_bytes) ||
				!decode_materials(
					material_bytes,
					sample_count,
					materials
				)))) {
		return 2;
	}
	wt::WtDenseGridSource source(sample_count);
	if (source.initialize(
			descriptor,
			std::move(densities),
			std::move(materials)
		) != wt::WtDenseGridStatus::Ok) {
		return 2;
	}
	std::vector<wt::WtChunkKey> keys;
	if (!read_keys(key_path, keys)) return 2;
	wt::WtChunkBaker baker(keys.size());
	std::vector<wt::WtBakedChunkPage> pages;
	if (baker.bake(keys, source_revision, source, pages) !=
		wt::WtChunkBakeStatus::Ok) {
		return 2;
	}
	wt::WtWorldManifest manifest;
	manifest.source_revision = source_revision;
	manifest.world_revision = 0;
	manifest.configuration_hash = configuration_hash;
	manifest.dependencies = {
		{
			wt::WtDependencyKind::SourceAsset,
			"density",
			"",
			wt::wt_sha256(density_bytes.data(), density_bytes.size()),
		},
		{
			wt::WtDependencyKind::Generator,
			"world-transvoxel-bake-tool",
			generator_version,
			hash_text(generator_version),
		},
		{
			wt::WtDependencyKind::Configuration,
			"dense-grid-configuration",
			"1",
			configuration_hash,
		},
		{
			wt::WtDependencyKind::Backend,
			"transvoxel-mit",
			"51a494f03c5b024cd153b596bcc7152eb3cc93a6",
			backend_hash,
		},
		{
			wt::WtDependencyKind::Godot,
			"godot-supported-matrix",
			godot_version,
			hash_text(godot_version),
		},
		{
			wt::WtDependencyKind::GodotCpp,
			"godot-cpp",
			godot_cpp_version,
			hash_text(godot_cpp_version),
		},
		{
			wt::WtDependencyKind::Toolchain,
			"zig",
			toolchain_version,
			hash_text(toolchain_version),
		},
	};
	if (material_argument == "-") {
		const std::uint8_t encoded_default[2] = {
			static_cast<std::uint8_t>(descriptor.default_material),
			static_cast<std::uint8_t>(descriptor.default_material >> 8),
		};
		manifest.dependencies.push_back({
			wt::WtDependencyKind::SourceAsset,
			"default-material",
			"",
			wt::wt_sha256(encoded_default, sizeof(encoded_default)),
		});
	} else {
		manifest.dependencies.push_back({
			wt::WtDependencyKind::SourceAsset,
			"materials",
			"",
			wt::wt_sha256(material_bytes.data(), material_bytes.size()),
		});
	}
	for (const wt::WtBakedChunkPage &page : pages) {
		manifest.pages.push_back({
			page.key,
			page.bytes.size(),
			page.content_hash,
		});
	}
	std::vector<std::uint8_t> world_bytes;
	if (wt::wt_write_world_manifest(manifest, world_bytes) !=
		wt::WtWorldManifestStatus::Ok) {
		return 2;
	}

	std::error_code error;
	if (std::filesystem::exists(output_path, error) || error ||
		output_path.filename().empty()) {
		return 2;
	}
	const std::filesystem::path temporary =
		output_path.parent_path() /
		(output_path.filename().string() + ".tmp");
	if (std::filesystem::exists(temporary, error) || error ||
		!std::filesystem::create_directories(temporary, error) || error) {
		return 2;
	}
	bool wrote_all = write_file(temporary / "world.wtworld", world_bytes);
	for (const wt::WtBakedChunkPage &page : pages) {
		wrote_all = wrote_all && write_file(
			temporary / (hash_hex(page.content_hash) + ".wtchunk"),
			page.bytes
		);
	}
	if (!wrote_all) {
		std::filesystem::remove_all(temporary, error);
		return 2;
	}
	std::filesystem::rename(temporary, output_path, error);
	if (error) {
		std::filesystem::remove_all(temporary, error);
		return 2;
	}
	std::printf(
		"{\"type\":\"wtbake\",\"pages\":%zu,\"source_revision\":%llu,"
		"\"world_sha256\":\"%s\"}\n",
		pages.size(),
		static_cast<unsigned long long>(source_revision),
		hash_hex(wt::wt_sha256(world_bytes.data(), world_bytes.size())).c_str()
	);
	return 0;
}

void usage() {
	std::fprintf(
		stderr,
		"usage: wt_bake_tool dense <density.f32le> <materials.u16le|-> "
		"<keys.txt> <output-dir> <origin-x> <origin-y> <origin-z> "
		"<dim-x> <dim-y> <dim-z> <spacing> <source-revision> "
		"<default-material> <config-hash> <backend-hash> "
		"<generator-version> <godot-version> <godot-cpp-version> "
		"<toolchain-version>\n"
	);
}

} // namespace

int main(int argc, char **argv) {
	if (argc >= 2 && std::string(argv[1]) == "dense") {
		const int status = bake_dense(argc, argv);
		if (status != 1) return status;
	}
	usage();
	return 1;
}
