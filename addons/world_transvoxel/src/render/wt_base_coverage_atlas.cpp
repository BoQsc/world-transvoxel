#include "render/wt_base_coverage_atlas.h"

#include "storage/wt_hash256.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <utility>

namespace world_transvoxel {
namespace {

constexpr std::size_t kHeaderBytes = 56;
constexpr std::size_t kRecordBytes = 60;
constexpr std::size_t kVertexBytes = 27;
constexpr std::size_t kMaximumAtlasBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumRoots = 4096;

bool read_u32(WtBinaryReader &reader, std::uint32_t &value) noexcept {
	return reader.read_u32(value) == WtBinaryStatus::Ok;
}

bool read_i32(WtBinaryReader &reader, std::int32_t &value) noexcept {
	return reader.read_i32(value) == WtBinaryStatus::Ok;
}

bool valid_vertex_payload(WtByteView bytes, std::uint32_t count) noexcept {
	for (std::size_t index = 0; index < count; ++index) {
		const std::uint8_t *vertex = bytes.data + index * kVertexBytes;
		for (std::size_t offset = 0; offset < 24; offset += 4) {
			const std::uint32_t bits =
				static_cast<std::uint32_t>(vertex[offset]) |
				(static_cast<std::uint32_t>(vertex[offset + 1]) << 8) |
				(static_cast<std::uint32_t>(vertex[offset + 2]) << 16) |
				(static_cast<std::uint32_t>(vertex[offset + 3]) << 24);
			float value = 0.0f;
			std::memcpy(&value, &bits, sizeof(value));
			if (!std::isfinite(value)) return false;
		}
		if (vertex[26] > 1) return false;
	}
	return true;
}

bool valid_indices(WtByteView bytes, std::uint32_t vertex_count) noexcept {
	for (std::size_t offset = 0; offset < bytes.size; offset += 4) {
		const std::uint32_t index =
			static_cast<std::uint32_t>(bytes.data[offset]) |
			(static_cast<std::uint32_t>(bytes.data[offset + 1]) << 8) |
			(static_cast<std::uint32_t>(bytes.data[offset + 2]) << 16) |
			(static_cast<std::uint32_t>(bytes.data[offset + 3]) << 24);
		if (index >= vertex_count) return false;
	}
	return true;
}

} // namespace

WtBaseCoverageAtlasStatus wt_open_base_coverage_atlas(
	WtByteView bytes,
	const WtProceduralWorldDescriptor &expected_source,
	std::uint8_t expected_lod,
	const std::vector<WtChunkKey> &expected_roots,
	WtBaseCoverageAtlasView &output
) noexcept {
	output = {};
	if (!bytes.data || expected_roots.empty() || expected_lod > kWtMaximumLod ||
		expected_source.world_revision != 0) return WtBaseCoverageAtlasStatus::InvalidInput;
	if (bytes.size > kMaximumAtlasBytes || expected_roots.size() > kMaximumRoots)
		return WtBaseCoverageAtlasStatus::CapacityExceeded;
	if (bytes.size < kHeaderBytes) return WtBaseCoverageAtlasStatus::Truncated;
	std::set<WtChunkKey> inventory;
	for (const WtChunkKey &key : expected_roots) {
		if (key.lod != expected_lod || !inventory.insert(key).second)
			return WtBaseCoverageAtlasStatus::InvalidInput;
	}
	WtBinaryReader reader(bytes);
	WtByteView magic;
	std::uint32_t version = 0;
	std::uint64_t source_revision = 0;
	std::uint32_t seed = 0, mode = 0, count_x = 0, count_y = 0, count_z = 0;
	std::int32_t origin_y = 0;
	std::uint32_t bottom_policy = 0, bottom_thickness = 0, lod = 0, root_count = 0;
	if (reader.read_bytes(4, magic) != WtBinaryStatus::Ok ||
		!read_u32(reader, version) || reader.read_u64(source_revision) != WtBinaryStatus::Ok ||
		!read_u32(reader, seed) || !read_u32(reader, mode) ||
		!read_u32(reader, count_x) || !read_u32(reader, count_y) ||
		!read_u32(reader, count_z) || !read_i32(reader, origin_y) ||
		!read_u32(reader, bottom_policy) || !read_u32(reader, bottom_thickness) ||
		!read_u32(reader, lod) || !read_u32(reader, root_count))
		return WtBaseCoverageAtlasStatus::Truncated;
	if (std::memcmp(magic.data, "WTBA", 4) != 0 || version != 1)
		return WtBaseCoverageAtlasStatus::UnsupportedVersion;
	if (source_revision != expected_source.source_revision || seed != expected_source.seed ||
		mode != static_cast<std::uint32_t>(expected_source.mode) ||
		count_x != expected_source.chunk_count_x || count_y != expected_source.chunk_count_y ||
		count_z != expected_source.chunk_count_z || origin_y != expected_source.chunk_y ||
		bottom_policy != static_cast<std::uint32_t>(expected_source.bottom_boundary_policy) ||
		bottom_thickness != expected_source.bottom_boundary_thickness_cells ||
		lod != expected_lod) return WtBaseCoverageAtlasStatus::SourceMismatch;
	if (root_count != expected_roots.size())
		return WtBaseCoverageAtlasStatus::IncompleteInventory;
	WtBaseCoverageAtlasView candidate;
	candidate.source_revision = source_revision;
	candidate.lod = expected_lod;
	candidate.roots.reserve(root_count);
	for (std::uint32_t root = 0; root < root_count; ++root) {
		if (reader.remaining() < kRecordBytes)
			return WtBaseCoverageAtlasStatus::Truncated;
		std::int32_t x = 0, y = 0, z = 0;
		std::uint32_t flags = 0, vertices = 0, indices = 0, payload_bytes = 0;
		WtByteView digest;
		if (!read_i32(reader, x) || !read_i32(reader, y) || !read_i32(reader, z) ||
			!read_u32(reader, flags) || !read_u32(reader, vertices) ||
			!read_u32(reader, indices) || !read_u32(reader, payload_bytes) ||
			reader.read_bytes(32, digest) != WtBinaryStatus::Ok)
			return WtBaseCoverageAtlasStatus::Truncated;
		const WtChunkKey key {x, y, z, expected_lod};
		if (inventory.erase(key) != 1 || flags > 1 ||
			static_cast<std::uint64_t>(vertices) * kVertexBytes +
				static_cast<std::uint64_t>(indices) * 4 != payload_bytes ||
			(flags == 1) != (vertices == 0 && indices == 0) ||
			indices % 3 != 0)
			return WtBaseCoverageAtlasStatus::InvalidRecord;
		WtByteView payload;
		if (reader.read_bytes(payload_bytes, payload) != WtBinaryStatus::Ok)
			return WtBaseCoverageAtlasStatus::Truncated;
		const auto hash = wt_sha256(payload.data, payload.size);
		if (std::memcmp(hash.data(), digest.data, hash.size()) != 0)
			return WtBaseCoverageAtlasStatus::HashMismatch;
		const WtByteView vertex_bytes {payload.data,
			static_cast<std::size_t>(vertices) * kVertexBytes};
		const WtByteView index_bytes {payload.data + vertex_bytes.size,
			static_cast<std::size_t>(indices) * 4};
		if (!valid_vertex_payload(vertex_bytes, vertices) ||
			!valid_indices(index_bytes, vertices))
			return WtBaseCoverageAtlasStatus::InvalidRecord;
		candidate.roots.push_back({key, vertex_bytes, index_bytes,
			vertices, indices, flags == 1});
	}
	if (!inventory.empty() || reader.remaining() != 0)
		return WtBaseCoverageAtlasStatus::IncompleteInventory;
	output = std::move(candidate);
	return WtBaseCoverageAtlasStatus::Ok;
}

} // namespace world_transvoxel
