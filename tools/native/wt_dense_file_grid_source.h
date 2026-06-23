#pragma once

#include "bake/wt_dense_grid_source.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace world_transvoxel {

enum class WtDenseFileGridStatus : std::uint8_t {
	Ok,
	InvalidInput,
	SizeOverflow,
	FileSizeMismatch,
	FileOpenFailure,
};

class WtDenseFileGridSource final : public WtChunkSampleSource {
public:
	WtDenseFileGridSource() noexcept = default;

	WtDenseFileGridStatus initialize(
		const WtDenseGridDescriptor &descriptor,
		const std::filesystem::path &density_path,
		const std::filesystem::path *material_path
	);

	bool sample(
		const WtGridPoint &point,
		WtScalarSample &output
	) const noexcept override;

	bool initialized() const noexcept;
	std::size_t sample_count() const noexcept;
	std::size_t cache_capacity_bytes() const noexcept;
	const WtDenseGridDescriptor &descriptor() const noexcept;

private:
	static constexpr std::size_t kBlockSamples = 64;
	static constexpr std::size_t kCacheSlots = 512;

	struct CacheEntry {
		std::size_t row = static_cast<std::size_t>(-1);
		std::size_t block = static_cast<std::size_t>(-1);
		std::size_t count = 0;
		std::array<std::uint8_t, kBlockSamples * 4> densities{};
		std::array<std::uint8_t, kBlockSamples * 2> materials{};
	};

	bool load_block(
		std::size_t row,
		std::size_t block,
		CacheEntry &entry
	) const noexcept;

	bool initialized_ = false;
	bool has_materials_ = false;
	std::size_t sample_count_ = 0;
	WtDenseGridDescriptor descriptor_;
	mutable std::ifstream density_;
	mutable std::ifstream materials_;
	mutable std::array<CacheEntry, kCacheSlots> cache_{};
};

} // namespace world_transvoxel
