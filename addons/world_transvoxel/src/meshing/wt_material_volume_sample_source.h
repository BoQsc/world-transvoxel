#pragma once

#include "meshing/wt_chunk_mesher.h"

#include <cstdint>

namespace world_transvoxel {

constexpr std::uint16_t kWtStaticWaterMaterialId = 9;

class WtMaterialVolumeSampleSource final : public WtChunkSampleSource {
public:
	WtMaterialVolumeSampleSource(
		const WtChunkSampleSource &source,
		std::uint16_t material,
		float solid_isovalue = 0.0F
	) noexcept;

	bool sample(
		const WtGridPoint &point,
		WtScalarSample &output
	) const noexcept override;
	WtMultiresolutionEdgeSourceStatus resolve_multiresolution_edge(
		const WtGridPoint &endpoint_a,
		const WtGridPoint &endpoint_b,
		float isovalue,
		WtResolvedMultiresolutionEdge &output
	) const noexcept override;

	static bool is_occupied(
		const WtScalarSample &sample,
		std::uint16_t material,
		float solid_isovalue = 0.0F
	) noexcept;

private:
	float free_surface_density(
		const WtGridPoint &point,
		const WtScalarSample &sample
	) const noexcept;
	const WtChunkSampleSource &source_;
	std::uint16_t material_ = 0;
	float solid_isovalue_ = 0.0F;
};

} // namespace world_transvoxel
