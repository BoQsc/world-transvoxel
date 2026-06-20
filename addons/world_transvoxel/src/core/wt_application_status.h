#pragma once

#include <cstdint>

namespace world_transvoxel {

enum class WtApplicationStatus : std::uint8_t {
	Ok,
	AlreadyCurrent,
	InvalidInput,
	NotFound,
	StaleGeneration,
	RecordCapacityExceeded,
	QueueFull,
};

} // namespace world_transvoxel
