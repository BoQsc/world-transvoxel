#include "api/world_transvoxel_terrain.h"

#include "render/wt_base_coverage_atlas.h"
#include "render/wt_base_coverage_gpu_pack.h"
#include "storage/wt_procedural_world_source.h"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace world_transvoxel {
namespace {

godot::PackedByteArray godot_bytes(const std::vector<std::uint8_t> &source) {
	godot::PackedByteArray result;
	result.resize(static_cast<std::int64_t>(source.size()));
	if (!source.empty()) std::memcpy(result.ptrw(), source.data(), source.size());
	return result;
}

const char *atlas_status_name(WtBaseCoverageAtlasStatus status) noexcept {
	switch (status) {
		case WtBaseCoverageAtlasStatus::Ok: return "ok";
		case WtBaseCoverageAtlasStatus::InvalidInput: return "invalid_input";
		case WtBaseCoverageAtlasStatus::CapacityExceeded: return "capacity_exceeded";
		case WtBaseCoverageAtlasStatus::Truncated: return "truncated";
		case WtBaseCoverageAtlasStatus::UnsupportedVersion: return "unsupported_version";
		case WtBaseCoverageAtlasStatus::SourceMismatch: return "source_mismatch";
		case WtBaseCoverageAtlasStatus::IncompleteInventory: return "incomplete_inventory";
		case WtBaseCoverageAtlasStatus::InvalidRecord: return "invalid_record";
		case WtBaseCoverageAtlasStatus::HashMismatch: return "hash_mismatch";
	}
	return "unknown";
}

} // namespace

godot::Dictionary WorldTransvoxelTerrain::_load_gpu_base_coverage_atlas(
	const godot::String &path
) {
	godot::Dictionary result;
	result["schema"] = "world_transvoxel.gpu_base_coverage_upload.v1";
	result["ok"] = false;
	result["error"] = "world is not a running pristine procedural source";
	WtProceduralWorldDescriptor source;
	if (!lifecycle_ || !lifecycle_->procedural_descriptor(source) ||
		source.world_revision != 0) return result;
	std::vector<WtChunkKey> expected;
	if (!wt_append_procedural_lod_keys(source, 3, expected, 4096) ||
		expected.empty()) {
		result["error"] = "source has no bounded LOD3 inventory";
		return result;
	}
	if (path.is_empty()) {
		result["error"] = "atlas path is empty";
		return result;
	}
	const godot::String absolute =
		godot::ProjectSettings::get_singleton()->globalize_path(path);
	const godot::CharString utf8 = absolute.utf8();
	const std::filesystem::path file_path = std::filesystem::u8path(utf8.get_data());
	std::error_code error;
	const auto size = std::filesystem::file_size(file_path, error);
	if (error || size == 0 || size > 64U * 1024U * 1024U) {
		result["error"] = "atlas file is missing or exceeds 64 MiB";
		return result;
	}
	std::ifstream input(file_path, std::ios::binary);
	if (!input) {
		result["error"] = "atlas file could not be opened";
		return result;
	}
	const std::vector<std::uint8_t> file {
		std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()
	};
	if (file.size() != size) {
		result["error"] = "atlas file changed during read";
		return result;
	}
	WtBaseCoverageAtlasView atlas;
	const auto status = wt_open_base_coverage_atlas(
		{file.data(), file.size()}, source, 3, expected, atlas
	);
	if (status != WtBaseCoverageAtlasStatus::Ok) {
		result["error"] = atlas_status_name(status);
		return result;
	}
	WtBaseCoverageGpuPack gpu;
	if (!wt_pack_base_coverage_for_gpu(atlas, gpu)) {
		result["error"] = "bounded GPU packing failed";
		return result;
	}
	std::sort(expected.begin(), expected.end());
	if (!lifecycle_->set_external_base_visual_cover(expected)) {
		result["error"] = "native base visual cover registration failed";
		return result;
	}
	godot::Array roots;
	for (const auto &root : gpu.roots) {
		godot::Dictionary item;
		item["key"] = godot::Vector3i(root.key.x, root.key.y, root.key.z);
		item["lod"] = static_cast<std::int64_t>(root.key.lod);
		item["draw_index"] = root.draw_index;
		roots.push_back(item);
	}
	result["positions"] = godot_bytes(gpu.positions);
	result["normals"] = godot_bytes(gpu.normals);
	result["metadata"] = godot_bytes(gpu.metadata);
	result["indices"] = godot_bytes(gpu.indices);
	result["indirect"] = godot_bytes(gpu.indirect);
	result["roots"] = roots;
	result["vertex_count"] = static_cast<std::int64_t>(gpu.vertex_count);
	result["index_count"] = static_cast<std::int64_t>(gpu.index_count);
	result["draw_count"] = static_cast<std::int64_t>(gpu.indirect.size() / 20);
	result["source_revision"] = static_cast<std::int64_t>(atlas.source_revision);
	result["world_revision"] = static_cast<std::int64_t>(source.world_revision);
	result["lod"] = static_cast<std::int64_t>(atlas.lod);
	result["error"] = "";
	result["ok"] = true;
	return result;
}

} // namespace world_transvoxel
