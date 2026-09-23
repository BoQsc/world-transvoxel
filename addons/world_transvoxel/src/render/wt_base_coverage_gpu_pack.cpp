#include "render/wt_base_coverage_gpu_pack.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace world_transvoxel {
namespace {

constexpr std::size_t kVertexBytes = 27;
constexpr std::size_t kMaximumPackedBytes = 64U * 1024U * 1024U;

std::uint32_t u32_at(const std::uint8_t *source) noexcept {
	return static_cast<std::uint32_t>(source[0]) |
		(static_cast<std::uint32_t>(source[1]) << 8) |
		(static_cast<std::uint32_t>(source[2]) << 16) |
		(static_cast<std::uint32_t>(source[3]) << 24);
}

float f32_at(const std::uint8_t *source) noexcept {
	const std::uint32_t bits = u32_at(source);
	float value = 0.0f;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

void append_u32(std::vector<std::uint8_t> &target, std::uint32_t value) {
	for (unsigned shift = 0; shift < 32; shift += 8)
		target.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_f32(std::vector<std::uint8_t> &target, float value) {
	std::uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(value));
	append_u32(target, bits);
}

void append_u16(std::vector<std::uint8_t> &target, std::uint16_t value) {
	target.push_back(static_cast<std::uint8_t>(value));
	target.push_back(static_cast<std::uint8_t>(value >> 8));
}

std::int16_t snorm16(float value) noexcept {
	const float clamped = std::max(-1.0f, std::min(1.0f, value));
	return static_cast<std::int16_t>(std::round(clamped * 32767.0f));
}

bool append_vertex(
	const std::uint8_t *source, const WtChunkKey &key,
	WtBaseCoverageGpuPack &output
) {
	const WtChunkBounds bounds = wt_chunk_bounds(key);
	for (std::size_t axis = 0; axis < 3; ++axis) {
		const std::int64_t origin = axis == 0 ? bounds.minimum.x :
			(axis == 1 ? bounds.minimum.y : bounds.minimum.z);
		const float position = f32_at(source + axis * 4) + static_cast<float>(origin);
		if (!std::isfinite(position)) return false;
		append_f32(output.positions, position);
	}
	float x = f32_at(source + 12);
	float y = f32_at(source + 16);
	float z = f32_at(source + 20);
	const float length = std::sqrt(x * x + y * y + z * z);
	if (!std::isfinite(length) || length < 1.0e-10f) return false;
	x /= length;
	y /= length;
	z /= length;
	const float inv_l1 = 1.0f / (std::abs(x) + std::abs(y) + std::abs(z));
	x *= inv_l1;
	y *= inv_l1;
	if (z < 0.0f) {
		const float old_x = x;
		x = (1.0f - std::abs(y)) * (old_x < 0.0f ? -1.0f : 1.0f);
		y = (1.0f - std::abs(old_x)) * (y < 0.0f ? -1.0f : 1.0f);
	}
	append_u16(output.normals, static_cast<std::uint16_t>(snorm16(x)));
	append_u16(output.normals, static_cast<std::uint16_t>(snorm16(y)));
	append_u16(output.metadata,
		static_cast<std::uint16_t>(source[24] | (source[25] << 8)));
	append_u16(output.metadata, source[26]);
	return true;
}

} // namespace

bool wt_pack_base_coverage_for_gpu(
	const WtBaseCoverageAtlasView &atlas,
	WtBaseCoverageGpuPack &output
) {
	output = {};
	if (atlas.roots.empty()) return false;
	std::uint64_t vertices = 0, indices = 0, draws = 0;
	for (const auto &root : atlas.roots) {
		if (root.key.lod != atlas.lod ||
			root.vertices.size != static_cast<std::size_t>(root.vertex_count) * kVertexBytes ||
			root.indices.size != static_cast<std::size_t>(root.index_count) * 4 ||
			(root.proven_empty != (root.vertex_count == 0 && root.index_count == 0)))
			return false;
		vertices += root.vertex_count;
		indices += root.index_count;
		draws += !root.proven_empty;
	}
	const std::uint64_t packed_bytes = vertices * 20 + indices * 4 + draws * 20;
	if (packed_bytes > kMaximumPackedBytes ||
		vertices > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
		indices > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) ||
		draws > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
		return false;
	WtBaseCoverageGpuPack candidate;
	candidate.positions.reserve(static_cast<std::size_t>(vertices) * 12);
	candidate.normals.reserve(static_cast<std::size_t>(vertices) * 4);
	candidate.metadata.reserve(static_cast<std::size_t>(vertices) * 4);
	candidate.indices.reserve(static_cast<std::size_t>(indices) * 4);
	candidate.indirect.reserve(static_cast<std::size_t>(draws) * 20);
	candidate.roots.reserve(atlas.roots.size());
	std::uint32_t first_index = 0;
	std::int32_t base_vertex = 0;
	std::int32_t draw_index = 0;
	for (const auto &root : atlas.roots) {
		WtBaseCoverageGpuRoot gpu_root;
		gpu_root.key = root.key;
		if (!root.proven_empty) {
			gpu_root.draw_index = draw_index++;
			for (std::uint32_t vertex = 0; vertex < root.vertex_count; ++vertex) {
				if (!append_vertex(root.vertices.data + vertex * kVertexBytes,
						root.key, candidate)) return false;
			}
			candidate.indices.insert(candidate.indices.end(),
				root.indices.data, root.indices.data + root.indices.size);
			append_u32(candidate.indirect, root.index_count);
			append_u32(candidate.indirect, 1); // visible until a complete cut replaces it
			append_u32(candidate.indirect, first_index);
			append_u32(candidate.indirect, static_cast<std::uint32_t>(base_vertex));
			append_u32(candidate.indirect, 0);
			first_index += root.index_count;
			base_vertex += static_cast<std::int32_t>(root.vertex_count);
		}
		candidate.roots.push_back(gpu_root);
	}
	candidate.vertex_count = static_cast<std::size_t>(vertices);
	candidate.index_count = static_cast<std::size_t>(indices);
	output = std::move(candidate);
	return true;
}

} // namespace world_transvoxel
