#pragma once

#include "core/wt_chunk_state.h"

#include <cstdint>

namespace world_transvoxel {

enum class WtPageMeshingRuntimeOwnerStatus : std::uint8_t {
	Ok,
	NotFound,
	StaleGeneration,
};

class WtPageMeshingRuntimeOwner {
public:
	virtual ~WtPageMeshingRuntimeOwner() = default;

	virtual WtPageMeshingRuntimeOwnerStatus cancel_owned_generation(
		const WtChunkKey &key,
		WtGenerationToken generation
	) = 0;
	virtual WtPageMeshingRuntimeOwnerStatus release_owned_chunk(
		const WtChunkKey &key
	) = 0;
	virtual WtPageMeshingRuntimeOwnerStatus reprioritize_owned_chunk(
		const WtChunkKey &key,
		WtGenerationToken generation,
		std::int32_t priority
	) = 0;
	// Collision roles freeze when CPU mesh preparation starts. Visual roles may
	// still reuse a retained immutable pre-mesh GPU field capture.
	virtual bool owned_generation_accepts_role_promotion(
		const WtChunkKey &key,
		WtGenerationToken generation,
		bool collision_promotion
	) const noexcept {
		(void)key;
		(void)generation;
		(void)collision_promotion;
		return false;
	}
};

} // namespace world_transvoxel
