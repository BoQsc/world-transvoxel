#pragma once

#include "backend/wt_meshing_backend.h"
#include "core/wt_chunk_key.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace world_transvoxel {

constexpr std::size_t kWtMaximumRegularChunkVertices = 49152;
constexpr std::size_t kWtMaximumRegularChunkIndices = 61440;
constexpr std::size_t kWtMaximumTransitionFaceVertices = 3072;
constexpr std::size_t kWtMaximumTransitionFaceIndices = 9216;
constexpr std::size_t kWtMaximumCachedScalarSamples = 131072;
constexpr std::size_t kWtMaximumCachedCellSamples = 32768;

struct WtScalarSample {
	float density = 0.0F;
	std::uint16_t material = 0;
};

class WtChunkSampleSource {
public:
	virtual ~WtChunkSampleSource() = default;
	virtual bool sample(const WtGridPoint &point, WtScalarSample &output) const noexcept = 0;
};

struct WtChunkMeshBuffer {
	std::vector<WtCellVertex> vertices;
	std::vector<std::uint32_t> indices;
	std::size_t vertex_limit = 0;
	std::size_t index_limit = 0;

	void prepare(std::size_t maximum_vertices, std::size_t maximum_indices);
	void clear() noexcept;
};

struct WtChunkMeshResult {
	WtChunkKey key;
	WtGridPoint world_origin;
	std::uint8_t transition_mask = 0;
	WtChunkMeshBuffer regular;
	std::array<WtChunkMeshBuffer, 6> transitions;

	void clear() noexcept;
};

struct WtGridPointHash {
	std::size_t operator()(const WtGridPoint &point) const noexcept;
};

struct WtChunkVertexKey {
	std::array<std::uint32_t, 6> vector_bits{};
	std::uint16_t material = 0;

	bool operator==(const WtChunkVertexKey &other) const noexcept;
};

struct WtChunkVertexKeyHash {
	std::size_t operator()(const WtChunkVertexKey &key) const noexcept;
};

struct WtChunkMeshingScratch {
	WtCellMeshingScratch cell;
	std::unordered_map<WtGridPoint, WtScalarSample, WtGridPointHash> scalar_samples;
	std::unordered_map<WtGridPoint, WtCellSample, WtGridPointHash> cell_samples;
	std::unordered_map<WtChunkVertexKey, std::uint32_t, WtChunkVertexKeyHash> vertices;

	WtChunkMeshingScratch();
	void reset_samples();
	void reset_vertices();
};

struct WtChunkMeshingInput {
	WtChunkKey key;
	std::uint8_t transition_mask = 0;
	float isovalue = 0.0F;
	float transition_width_ratio = 0.25F;
};

enum class WtChunkMeshingStatus : std::uint8_t {
	Ok,
	InvalidInput,
	SampleSourceFailure,
	SampleCacheOverflow,
	RegularBufferOverflow,
	TransitionBufferOverflow,
	CellBackendFailure,
};

class WtChunkMesher {
public:
	explicit WtChunkMesher(const WtMeshingBackend &backend) noexcept;

	WtChunkMeshingStatus mesh(
		const WtChunkMeshingInput &input,
		const WtChunkSampleSource &source,
		WtChunkMeshResult &output,
		WtChunkMeshingScratch &scratch
	) const;

private:
	const WtMeshingBackend &backend_;
};

} // namespace world_transvoxel
