#include "storage/wt_procedural_world_source.h"

#include "bake/wt_chunk_baker.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

std::uint32_t vertical_layer_count(
	const WtProceduralWorldDescriptor &descriptor,
	std::uint8_t lod
) noexcept {
	return ceil_divide_u32(descriptor.chunk_count_y, lod_span(lod));
}

std::int32_t vertical_origin(
	const WtProceduralWorldDescriptor &descriptor,
	std::uint8_t lod
) noexcept {
	return floor_divide_i32(descriptor.chunk_y, static_cast<std::int32_t>(lod_span(lod)));
}

float regularized_density(double density) noexcept {
	constexpr double kDensityEpsilon = 0.01;
	if (!std::isfinite(density)) {
		return std::numeric_limits<float>::quiet_NaN();
	}
	if (density > -kDensityEpsilon && density < kDensityEpsilon) {
		return density < 0.0 ? -static_cast<float>(kDensityEpsilon) :
			static_cast<float>(kDensityEpsilon);
	}
	return static_cast<float>(density);
}

double procedural_wave(
	std::int64_t x,
	std::int64_t y,
	std::int64_t z,
	double seed_phase
) noexcept {
	return
		std::sin(static_cast<double>(x) * 0.031 + seed_phase * 1.73) *
			0.47 +
		std::cos(static_cast<double>(z) * 0.027 - seed_phase * 1.19) *
			0.38 +
		std::sin(static_cast<double>(x + z) * 0.014 +
			static_cast<double>(y) * 0.041 + seed_phase) * 0.29;
}

bool underground_ore_patch(
	std::int64_t x,
	std::int64_t y,
	std::int64_t z,
	double seed_phase
) noexcept {
	const double vein =
		std::sin(static_cast<double>(x) * 0.073 + seed_phase) *
		std::cos(static_cast<double>(y) * 0.097 - seed_phase * 0.7) *
		std::sin(static_cast<double>(z) * 0.061 + seed_phase * 1.3);
	const double pocket = procedural_wave(x, y, z, seed_phase * 0.37);
	return vein > 0.48 && pocket > 0.08;
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
		output.density = regularized_density(static_cast<double>(point.y) - surface);
		output.material = material(surface, point.x, point.y, point.z);
		return std::isfinite(output.density);
	}

private:
	double height(std::int64_t x, std::int64_t z) const noexcept {
		if (descriptor_.mode == WtProceduralWorldMode::Flat) {
			return 8.0;
		}
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
		const double radial_distance =
			normalized_x * normalized_x + normalized_z * normalized_z;
		const double central_highland = 18.0 * std::exp(
			-2.1 * radial_distance
		);
		const double ridge_axis = normalized_z + normalized_x * 0.34 - 0.06;
		const double ridge_along = normalized_x - 0.05;
		const double mountain_range = 18.0 *
			std::exp(-48.0 * ridge_axis * ridge_axis) *
			std::exp(-1.2 * ridge_along * ridge_along);
		const double spire_a = 50.0 * std::exp(
			-165.0 * (
				(normalized_x - 0.16) * (normalized_x - 0.16) +
				(normalized_z + 0.02) * (normalized_z + 0.02)
			)
		);
		const double spire_b = 38.0 * std::exp(
			-170.0 * (
				(normalized_x + 0.16) * (normalized_x + 0.16) +
				(normalized_z - 0.19) * (normalized_z - 0.19)
			)
		);
		const double spire_c = 34.0 * std::exp(
			-190.0 * (
				(normalized_x - 0.36) * (normalized_x - 0.36) +
				(normalized_z + 0.30) * (normalized_z + 0.30)
			)
		);
		const double knife_ridge = 20.0 *
			std::exp(-95.0 * (
				normalized_z + 0.22 * normalized_x + 0.13
			) * (
				normalized_z + 0.22 * normalized_x + 0.13
			)) *
			std::exp(-3.0 * (normalized_x - 0.25) * (normalized_x - 0.25));
		const double cliff = 10.0 /
			(1.0 + std::exp(-35.0 * (
				0.20 - normalized_z + 0.18 * normalized_x
			))) *
			std::exp(-1.8 * (normalized_x - 0.18) * (normalized_x - 0.18));
		const double basin = -12.0 * std::exp(
			-5.0 * (
				(normalized_x + 0.44) * (normalized_x + 0.44) +
				(normalized_z - 0.27) * (normalized_z - 0.27)
			)
		);
		const double macro =
			4.0 * std::sin(static_cast<double>(x) * 0.0032 + phase * 1.7) +
			3.2 * std::cos(static_cast<double>(z) * 0.0038 - phase * 1.3) +
			2.3 * std::sin(static_cast<double>(x + z) * 0.0024 + phase);
		const double hills =
			2.6 * std::sin(static_cast<double>(x) * 0.010 + phase) *
				std::cos(static_cast<double>(z) * 0.0085 - phase * 0.5) +
			1.6 * std::cos(static_cast<double>(x - z) * 0.0075 - phase * 0.25);
		const double crag =
			std::sin(static_cast<double>(x) * 0.045 + phase) *
			std::cos(static_cast<double>(z) * 0.041 - phase * 0.5);
		const double crags = 6.0 * std::max(0.0, crag) *
			std::max(0.0, crag) * std::exp(-2.1 * radial_distance);
		const double long_wave =
			1.0 * std::sin(static_cast<double>(x) * 0.016 + phase) +
			0.8 * std::cos(static_cast<double>(z) * 0.014 - phase);
		const double local = 0.45 * std::cos(
			static_cast<double>(x - z) * 0.021 - phase * 0.25
		);
		return 12.0 + central_highland + mountain_range + spire_a +
			spire_b + spire_c + knife_ridge + cliff + basin + macro +
			hills + crags + long_wave + local;
	}

	std::uint16_t material(
		double surface,
		std::int64_t x,
		std::int64_t y,
		std::int64_t z
	) const noexcept {
		const double seed_phase =
			static_cast<double>(descriptor_.seed % 100000U) * 0.0001;
		const double depth = surface - static_cast<double>(y);
		if (depth >= 12.0 && underground_ore_patch(x, y, z, seed_phase)) {
			return 8;
		}
		if (depth >= 8.0) return 1;

		const double macro_biome =
			std::sin(static_cast<double>(x) * 0.0042 + seed_phase * 1.5) +
			std::cos(static_cast<double>(z) * 0.0037 - seed_phase * 0.8) +
			0.55 * std::sin(static_cast<double>(x - z) * 0.0021 + seed_phase);
		const double dry_biome =
			std::sin(static_cast<double>(x + z) * 0.0051 - seed_phase * 0.4) +
			0.42 * std::cos(static_cast<double>(x) * 0.0063 + seed_phase);
		if (surface > 40.0 || (surface > 30.0 && macro_biome > 0.35)) {
			return 5;
		}
		if (surface < 10.0 || dry_biome > 0.85) {
			return 4;
		}
		if (surface > 22.0 || macro_biome < -0.70) {
			return 3;
		}
		return 2;
	}

	WtProceduralWorldDescriptor descriptor_;
};

} // namespace

bool wt_valid_procedural_descriptor(
	const WtProceduralWorldDescriptor &descriptor
) noexcept {
	const std::uint64_t page_count = wt_procedural_page_count(descriptor);
	return descriptor.chunk_count_x != 0 &&
		descriptor.chunk_count_y != 0 &&
		descriptor.chunk_count_z != 0 &&
		descriptor.source_revision != 0 &&
		page_count != 0 &&
		page_count <= kWtMaximumProceduralPageCount;
}

std::uint64_t wt_procedural_page_count(
	const WtProceduralWorldDescriptor &descriptor
) noexcept {
	if (
		descriptor.chunk_count_x == 0 ||
		descriptor.chunk_count_y == 0 ||
		descriptor.chunk_count_z == 0
	) {
		return 0;
	}
	std::uint64_t pages = 0;
	for (std::uint8_t lod = 0; lod <= kWtProceduralMaximumLod; ++lod) {
		const std::uint32_t span = lod_span(lod);
		pages += static_cast<std::uint64_t>(
			ceil_divide_u32(descriptor.chunk_count_x, span)
		) * static_cast<std::uint64_t>(
			ceil_divide_u32(descriptor.chunk_count_z, span)
		) * static_cast<std::uint64_t>(
			vertical_layer_count(descriptor, lod)
		);
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
		const std::uint32_t count_y = vertical_layer_count(descriptor, lod);
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
		min_y + static_cast<std::int32_t>(vertical_layer_count(descriptor, key.lod))
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
