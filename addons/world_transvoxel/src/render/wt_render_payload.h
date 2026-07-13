#pragma once

#include "core/wt_chunk_state.h"
#include "meshing/wt_chunk_mesher.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

constexpr std::size_t kWtMaximumRenderVertices =
	kWtMaximumRegularChunkVertices + 6 * kWtMaximumTransitionFaceVertices;
constexpr std::size_t kWtMaximumRenderIndices =
	kWtMaximumRegularChunkIndices + 6 * kWtMaximumTransitionFaceIndices;

struct WtRenderVertex {
	WtVec3 position;
	WtVec3 normal;
	std::uint16_t material = 0;
};

struct WtRenderPayload {
	WtChunkKey key;
	WtGenerationToken generation;
	WtGridPoint world_origin;
	std::uint8_t transition_mask = 0;
	std::vector<WtRenderVertex> vertices;
	std::vector<std::uint32_t> indices;

	WtRenderPayload();
	void clear() noexcept;
};

enum class WtRenderBuildStatus : std::uint8_t {
	Ok,
	InvalidInput,
	InvalidMesh,
	CapacityExceeded,
};

WtRenderBuildStatus wt_build_render_payload(
	const WtChunkMeshResult &mesh,
	WtGenerationToken generation,
	WtRenderPayload &output
);

} // namespace world_transvoxel
