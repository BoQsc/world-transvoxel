#pragma once

#include "core/wt_chunk_key.h"

#include <cstdint>

namespace world_transvoxel {

struct WtGenerationToken {
	std::uint64_t value = 0;

	bool operator==(const WtGenerationToken &other) const noexcept {
		return value == other.value;
	}

	bool operator!=(const WtGenerationToken &other) const noexcept {
		return value != other.value;
	}
};

enum class WtChunkLifecycle : std::uint8_t {
	Requested,
	Sampling,
	Meshing,
	Ready,
	Cancelled,
	Failed,
};

struct WtChunkRecord {
	WtChunkKey key;
	WtGenerationToken generation;
	std::uint64_t source_revision = 0;
	std::uint64_t world_revision = 0;
	std::int32_t priority = 0;
	WtChunkLifecycle lifecycle = WtChunkLifecycle::Requested;
	std::uint8_t transition_mask = 0;
};

struct WtViewerSnapshot {
	std::uint64_t id = 0;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	std::uint64_t revision = 0;
};

} // namespace world_transvoxel
