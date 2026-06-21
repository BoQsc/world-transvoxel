#include "wt_production_world_fixture.h"

#include "storage/wt_hash256.h"
#include "storage/wt_world_manifest.h"

#include <fstream>
#include <string>
#include <vector>

namespace world_transvoxel::testing {
namespace {

WtHash256 hash_text(const char *text) {
	const std::string value(text);
	return wt_sha256(
		reinterpret_cast<const std::uint8_t *>(value.data()),
		value.size()
	);
}

WtDependencyEntry dependency(
	WtDependencyKind kind,
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
	if (!output) return false;
	output.write(
		reinterpret_cast<const char *>(bytes.data()),
		static_cast<std::streamsize>(bytes.size())
	);
	return output.good();
}

} // namespace

bool wt_write_production_world_fixture(
	const std::filesystem::path &root,
	std::uint64_t source_revision,
	std::uint64_t world_revision,
	std::filesystem::path &world_manifest_path
) {
	world_manifest_path.clear();
	std::error_code error;
	if ((!std::filesystem::exists(root, error) &&
			!std::filesystem::create_directories(root, error)) || error ||
		!std::filesystem::is_directory(root, error) || error) {
		return false;
	}
	WtWorldManifest manifest;
	manifest.source_revision = source_revision;
	manifest.world_revision = world_revision;
	manifest.configuration_hash = hash_text("production-lifecycle-config-v1");
	manifest.dependencies = {
		dependency(WtDependencyKind::SourceAsset,
			"empty-production-fixture", "", "empty-source-v1"),
		dependency(WtDependencyKind::Generator,
			"world-transvoxel-baker", "0.6.0-m5", "fixture-generator-v1"),
		dependency(WtDependencyKind::Configuration,
			"production-lifecycle", "1", "production-lifecycle-config-v1"),
		dependency(WtDependencyKind::Backend,
			"transvoxel-mit", "51a494f0", "fixture-backend-v1"),
		dependency(WtDependencyKind::Godot,
			"godot", "4.6.3", "fixture-godot-v1"),
		dependency(WtDependencyKind::GodotCpp,
			"godot-cpp", "e83fd090", "fixture-godot-cpp-v1"),
		dependency(WtDependencyKind::Toolchain,
			"zig", "0.16.0", "fixture-zig-v1"),
	};
	std::vector<std::uint8_t> bytes;
	if (wt_write_world_manifest(manifest, bytes) !=
		WtWorldManifestStatus::Ok) {
		return false;
	}
	world_manifest_path = root / "world.wtworld";
	return write_file(world_manifest_path, bytes);
}

} // namespace world_transvoxel::testing
