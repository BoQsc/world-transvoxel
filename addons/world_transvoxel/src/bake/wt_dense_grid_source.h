#pragma once

#include "meshing/wt_chunk_mesher.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

struct WtDenseGridDescriptor {
	WtGridPoint origin;
	std::uint32_t dimension_x = 0;
	std::uint32_t dimension_y = 0;
	std::uint32_t dimension_z = 0;
	std::uint64_t spacing = 1;
	std::uint16_t default_material = 0;
};

enum class WtDenseGridStatus : std::uint8_t {
	Ok,
	InvalidInput,
	SampleCapacityExceeded,
	NonFiniteDensity,
};

class WtDenseGridSource final : public WtChunkSampleSource {
public:
	explicit WtDenseGridSource(std::size_t sample_capacity) noexcept;

	WtDenseGridStatus initialize(
		const WtDenseGridDescriptor &descriptor,
		std::vector<float> densities,
		std::vector<std::uint16_t> materials
	);

	bool sample(
		const WtGridPoint &point,
		WtScalarSample &output
	) const noexcept override;

	bool initialized() const noexcept;
	std::size_t sample_count() const noexcept;
	const WtDenseGridDescriptor &descriptor() const noexcept;

private:
	std::size_t sample_capacity_ = 0;
	bool initialized_ = false;
	WtDenseGridDescriptor descriptor_;
	std::vector<float> densities_;
	std::vector<std::uint16_t> materials_;
};

} // namespace world_transvoxel
