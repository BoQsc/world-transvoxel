#pragma once

#include "storage/wt_async_storage_service.h"

#include <cstdint>
#include <vector>

namespace world_transvoxel {

constexpr std::uint64_t kWtMaximumProceduralPageCount = 262144;
constexpr std::uint8_t kWtProceduralMaximumLod = 1;

bool wt_valid_procedural_descriptor(
	const WtProceduralWorldDescriptor &descriptor
) noexcept;

std::uint64_t wt_procedural_page_count(
	const WtProceduralWorldDescriptor &descriptor
) noexcept;

std::vector<WtChunkKey> wt_procedural_keys(
	const WtProceduralWorldDescriptor &descriptor
);

bool wt_procedural_has_key(
	const std::vector<WtChunkKey> &keys,
	const WtChunkKey &key
) noexcept;

WtPageLoadCompletion wt_generate_procedural_page(
	const WtProceduralWorldDescriptor &descriptor,
	const WtChunkKey &key,
	WtGenerationToken generation,
	std::uint64_t &bytes_read
);

} // namespace world_transvoxel
