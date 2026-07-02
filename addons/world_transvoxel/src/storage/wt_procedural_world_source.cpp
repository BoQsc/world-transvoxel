#include "storage/wt_procedural_world_source.h"

#include "bake/wt_chunk_baker.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace world_transvoxel {
namespace {

std::uint32_t ceil_divide_u32(
	std::uint32_t value,
	std::uint32_t divisor
) noexcept {
	return (value + divisor - 1U) / divisor;
}

std::int32_t floor_divide_i32(
	std::int32_t value,
	std::int32_t divisor
) noexcept {
	return value >= 0 ? value / divisor :
		-static_cast<std::int32_t>(
			(-static_cast<std::int64_t>(value) + divisor - 1) / divisor
		);
}

std::uint32_t lod_span(std::uint8_t lod) noexcept {
	return std::uint32_t{ 1 } << lod;
}

std::uint32_t vertical_layer_count(std::uint8_t lod) noexcept {
	return std::uint32_t{ 1 } << (kWtProceduralMaximumLod - lod);
}

std::int32_t vertical_origin(
	const WtProceduralWorldDescriptor &descriptor,
	std::uint8_t lod
) noexcept {
	return floor_divide_i32(descriptor.chunk_y, static_cast<std::int32_t>(lod_span(lod)));
}

class WtProceduralTerrainVolumeSource final : public WtChunkSampleSource {
public:
	explicit WtProceduralTerrainVolumeSource(
		WtProceduralWorldDescriptor descriptor
	) noexcept :
			descriptor_(descriptor) {
	}

	bool sample(
		const WtGridPoint &point,
		WtScalarSample &output
	) const noexcept override {
		const double surface = height(point.x, point.z);
		output.density = static_cast<float>(
			static_cast<double>(point.y) - surface
		);
		output.material = material(surface, point.x, point.y, point.z);
		return std::isfinite(output.density);
	}

private:
	double height(std::int64_t x, std::int64_t z) const noexcept {
		const double width = std::max(
			16.0,
			static_cast<double>(descriptor_.chunk_count_x) *
				static_cast<double>(kWtChunkCellsPerAxis)
		);
		const double depth = std::max(
			16.0,
			static_cast<double>(descriptor_.chunk_count_z) *
				static_cast<double>(kWtChunkCellsPerAxis)
		);
		const double center_x = width * 0.5 - 0.5;
		const double center_z = depth * 0.5 - 0.5;
		const double normalized_x =
			(static_cast<double>(x) - center_x) / std::max(center_x, 1.0);
		const double normalized_z =
			(static_cast<double>(z) - center_z) / std::max(center_z, 1.0);
		const double phase =
			static_cast<double>(descriptor_.seed % 100000U) * 0.0001;
		const double ridge = 3.2 * std::exp(
			-3.0 * (normalized_x * normalized_x + normalized_z * normalized_z)
		);
		const double long_wave =
			0.80 * std::sin(static_cast<double>(x) * 0.018 + phase) +
			0.60 * std::cos(static_cast<double>(z) * 0.016 - phase);
		const double diagonal = 0.40 * std::sin(
			static_cast<double>(x + z) * 0.008 + phase * 0.5
		);
		const double local = 0.28 * std::cos(
			static_cast<double>(x - z) * 0.021 - phase * 0.25
		);
		return 5.8 + ridge + long_wave + diagonal + local;
	}

	std::uint16_t material(
		double surface,
		std::int64_t x,
		std::int64_t y,
		std::int64_t z
	) const noexcept {
		const double depth = surface - static_cast<double>(y);
		if (depth >= 8.0) return 1;
		if (depth >= 3.0) return 7;
		if (depth >= 1.0) return 4;
		if (surface < 7.6) return 2;
		if (surface > 11.0) return 7;
		const std::int64_t band =
			(x >= 0 ? x / 96 : (x - 95) / 96) +
			(z >= 0 ? z / 96 : (z - 95) / 96);
		return band % 3 == 0 ? 4 : 3;
	}

	WtProceduralWorldDescriptor descriptor_;
};

} // namespace

bool wt_valid_procedural_descriptor(
	const WtProceduralWorldDescriptor &descriptor
) noexcept {
	const std::uint64_t page_count = wt_procedural_page_count(descriptor);
	return descriptor.chunk_count_x != 0 &&
		descriptor.chunk_count_z != 0 &&
		descriptor.source_revision != 0 &&
		page_count != 0 &&
		page_count <= kWtMaximumProceduralPageCount;
}

std::uint64_t wt_procedural_page_count(
	const WtProceduralWorldDescriptor &descriptor
) noexcept {
	if (descriptor.chunk_count_x == 0 || descriptor.chunk_count_z == 0) return 0;
	std::uint64_t pages = 0;
	for (std::uint8_t lod = 0; lod <= kWtProceduralMaximumLod; ++lod) {
		const std::uint32_t span = lod_span(lod);
		pages += static_cast<std::uint64_t>(
			ceil_divide_u32(descriptor.chunk_count_x, span)
		) * static_cast<std::uint64_t>(
			ceil_divide_u32(descriptor.chunk_count_z, span)
		) * static_cast<std::uint64_t>(vertical_layer_count(lod));
	}
	return pages;
}

std::vector<WtChunkKey> wt_procedural_keys(
	const WtProceduralWorldDescriptor &descriptor
) {
	std::vector<WtChunkKey> keys;
	keys.reserve(static_cast<std::size_t>(wt_procedural_page_count(descriptor)));
	for (std::uint8_t lod = 0; lod <= kWtProceduralMaximumLod; ++lod) {
		const std::uint32_t span = lod_span(lod);
		const std::uint32_t count_x = ceil_divide_u32(descriptor.chunk_count_x, span);
		const std::uint32_t count_z = ceil_divide_u32(descriptor.chunk_count_z, span);
		const std::uint32_t count_y = vertical_layer_count(lod);
		const std::int32_t origin_y = vertical_origin(descriptor, lod);
		for (std::uint32_t z = 0; z < count_z; ++z) {
			for (std::uint32_t y = 0; y < count_y; ++y) {
				for (std::uint32_t x = 0; x < count_x; ++x) {
					keys.push_back({
						static_cast<std::int32_t>(x),
						static_cast<std::int32_t>(origin_y + static_cast<std::int32_t>(y)),
						static_cast<std::int32_t>(z),
						lod,
					});
				}
			}
		}
	}
	std::sort(keys.begin(), keys.end());
	return keys;
}

bool wt_procedural_has_key(
	const std::vector<WtChunkKey> &keys,
	const WtChunkKey &key
) noexcept {
	return std::binary_search(keys.begin(), keys.end(), key);
}

bool wt_procedural_can_generate_page(
	const WtProceduralWorldDescriptor &descriptor,
	const WtChunkKey &key
) noexcept {
	if (!wt_is_valid_chunk_key(key) || key.lod > kWtProceduralMaximumLod) {
		return false;
	}
	const std::uint32_t span = lod_span(key.lod);
	const std::int32_t count_x = static_cast<std::int32_t>(
		ceil_divide_u32(descriptor.chunk_count_x, span)
	);
	const std::int32_t count_z = static_cast<std::int32_t>(
		ceil_divide_u32(descriptor.chunk_count_z, span)
	);
	const std::int32_t min_y = vertical_origin(descriptor, key.lod);
	const std::int32_t max_y = static_cast<std::int32_t>(
		min_y + static_cast<std::int32_t>(vertical_layer_count(key.lod))
	);
	return key.x >= -1 &&
		key.x <= count_x &&
		key.z >= -1 &&
		key.z <= count_z &&
		key.y >= min_y - 1 &&
		key.y <= max_y;
}

WtPageLoadCompletion wt_generate_procedural_page(
	const WtProceduralWorldDescriptor &descriptor,
	const WtChunkKey &key,
	WtGenerationToken generation,
	std::uint64_t &bytes_read
) {
	WtPageLoadCompletion completion;
	completion.key = key;
	completion.generation = generation;
	if (!wt_procedural_can_generate_page(descriptor, key)) {
		completion.status = WtPageLoadStatus::PageFailure;
		return completion;
	}
	WtProceduralTerrainVolumeSource source(descriptor);
	std::vector<WtBakedChunkPage> pages;
	const WtChunkBaker baker(1);
	if (baker.bake({ key }, descriptor.source_revision, source, pages) !=
			WtChunkBakeStatus::Ok ||
		pages.size() != 1) {
		completion.status = WtPageLoadStatus::PageFailure;
		return completion;
	}
	auto bytes = std::make_shared<std::vector<std::uint8_t>>(
		std::move(pages[0].bytes)
	);
	bytes_read = bytes->size();
	completion.status = WtPageLoadStatus::Ok;
	completion.page_bytes = std::move(bytes);
	return completion;
}

} // namespace world_transvoxel
