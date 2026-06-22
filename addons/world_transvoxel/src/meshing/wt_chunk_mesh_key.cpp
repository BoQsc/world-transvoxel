#include "meshing/wt_chunk_mesh_key.h"

#include <cstring>

namespace world_transvoxel {
namespace {

std::size_t mix_hash(std::size_t hash, std::uint64_t value) noexcept {
	value ^= value >> 30;
	value *= 0xbf58476d1ce4e5b9ULL;
	value ^= value >> 27;
	value *= 0x94d049bb133111ebULL;
	value ^= value >> 31;
	return hash ^ static_cast<std::size_t>(
		value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2)
	);
}

std::uint32_t float_bits(float value) noexcept {
	std::uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

}

WtChunkVertexKey wt_make_chunk_vertex_key(const WtCellVertex &vertex) noexcept {
	return {
		{
			float_bits(vertex.position.x),
			float_bits(vertex.position.y),
			float_bits(vertex.position.z),
			float_bits(vertex.normal.x),
			float_bits(vertex.normal.y),
			float_bits(vertex.normal.z),
		},
		vertex.material,
	};
}

std::size_t WtGridPointHash::operator()(const WtGridPoint &point) const noexcept {
	std::size_t hash = 0;
	hash = mix_hash(hash, static_cast<std::uint64_t>(point.x));
	hash = mix_hash(hash, static_cast<std::uint64_t>(point.y));
	return mix_hash(hash, static_cast<std::uint64_t>(point.z));
}

bool WtChunkVertexKey::operator==(const WtChunkVertexKey &other) const noexcept {
	return vector_bits == other.vector_bits && material == other.material;
}

std::size_t WtChunkVertexKeyHash::operator()(
	const WtChunkVertexKey &key
) const noexcept {
	std::size_t hash = 0;
	for (std::uint32_t bits : key.vector_bits) {
		hash = mix_hash(hash, bits);
	}
	return mix_hash(hash, key.material);
}

}
