#include "storage/wt_procedural_world_source.h"

#include "bake/wt_chunk_baker.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace world_transvoxel {
namespace {

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
	const std::uint64_t page_count =
		static_cast<std::uint64_t>(descriptor.chunk_count_x) *
		static_cast<std::uint64_t>(descriptor.chunk_count_z);
	return descriptor.chunk_count_x != 0 &&
		descriptor.chunk_count_z != 0 &&
		descriptor.source_revision != 0 &&
		page_count != 0 &&
		page_count <= kWtMaximumProceduralPageCount;
}

std::vector<WtChunkKey> wt_procedural_keys(
	const WtProceduralWorldDescriptor &descriptor
) {
	std::vector<WtChunkKey> keys;
	keys.reserve(
		static_cast<std::size_t>(descriptor.chunk_count_x) *
		static_cast<std::size_t>(descriptor.chunk_count_z)
	);
	for (std::uint32_t z = 0; z < descriptor.chunk_count_z; ++z) {
		for (std::uint32_t x = 0; x < descriptor.chunk_count_x; ++x) {
			keys.push_back({
				static_cast<std::int32_t>(x),
				descriptor.chunk_y,
				static_cast<std::int32_t>(z),
				0,
			});
		}
	}
	return keys;
}

bool wt_procedural_has_key(
	const std::vector<WtChunkKey> &keys,
	const WtChunkKey &key
) noexcept {
	return std::binary_search(keys.begin(), keys.end(), key);
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
	if (!wt_is_valid_chunk_key(key) || key.lod != 0 ||
		key.y != descriptor.chunk_y ||
		key.x < 0 || key.z < 0 ||
		static_cast<std::uint32_t>(key.x) >= descriptor.chunk_count_x ||
		static_cast<std::uint32_t>(key.z) >= descriptor.chunk_count_z) {
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
