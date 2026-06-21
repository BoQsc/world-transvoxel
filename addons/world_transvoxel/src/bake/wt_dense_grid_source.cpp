#include "bake/wt_dense_grid_source.h"

#include <cmath>
#include <limits>
#include <utility>

namespace world_transvoxel {
namespace {

bool multiply_fits(
	std::size_t left,
	std::size_t right,
	std::size_t maximum,
	std::size_t &output
) noexcept {
	if (left != 0 && right > maximum / left) return false;
	output = left * right;
	return true;
}

bool axis_index(
	std::int64_t point,
	std::int64_t origin,
	std::uint64_t spacing,
	std::uint32_t dimension,
	std::size_t &output
) noexcept {
	const __int128_t delta =
		static_cast<__int128_t>(point) - static_cast<__int128_t>(origin);
	if (delta < 0 ||
		delta % static_cast<__int128_t>(spacing) != 0) {
		return false;
	}
	const __uint128_t index =
		static_cast<__uint128_t>(delta) / spacing;
	if (index >= dimension) return false;
	output = static_cast<std::size_t>(index);
	return true;
}

} // namespace

WtDenseGridSource::WtDenseGridSource(
	std::size_t sample_capacity
) noexcept :
		sample_capacity_(sample_capacity) {
}

WtDenseGridStatus WtDenseGridSource::initialize(
	const WtDenseGridDescriptor &descriptor,
	std::vector<float> densities,
	std::vector<std::uint16_t> materials
) {
	initialized_ = false;
	descriptor_ = {};
	densities_.clear();
	materials_.clear();
	if (descriptor.dimension_x == 0 ||
		descriptor.dimension_y == 0 ||
		descriptor.dimension_z == 0 ||
		descriptor.spacing == 0 ||
		descriptor.spacing >
			static_cast<std::uint64_t>(
				std::numeric_limits<std::int64_t>::max()
			)) {
		return WtDenseGridStatus::InvalidInput;
	}
	std::size_t plane = 0;
	std::size_t count = 0;
	if (!multiply_fits(
			descriptor.dimension_x,
			descriptor.dimension_y,
			sample_capacity_,
			plane
		) ||
		!multiply_fits(
			plane,
			descriptor.dimension_z,
			sample_capacity_,
			count
		) ||
		count > sample_capacity_) {
		return WtDenseGridStatus::SampleCapacityExceeded;
	}
	if (densities.size() != count ||
		(!materials.empty() && materials.size() != count)) {
		return WtDenseGridStatus::InvalidInput;
	}
	for (float density : densities) {
		if (!std::isfinite(density)) {
			return WtDenseGridStatus::NonFiniteDensity;
		}
	}
	descriptor_ = descriptor;
	densities_ = std::move(densities);
	materials_ = std::move(materials);
	initialized_ = true;
	return WtDenseGridStatus::Ok;
}

bool WtDenseGridSource::sample(
	const WtGridPoint &point,
	WtScalarSample &output
) const noexcept {
	if (!initialized_) return false;
	std::size_t x = 0;
	std::size_t y = 0;
	std::size_t z = 0;
	if (!axis_index(
			point.x,
			descriptor_.origin.x,
			descriptor_.spacing,
			descriptor_.dimension_x,
			x
		) ||
		!axis_index(
			point.y,
			descriptor_.origin.y,
			descriptor_.spacing,
			descriptor_.dimension_y,
			y
		) ||
		!axis_index(
			point.z,
			descriptor_.origin.z,
			descriptor_.spacing,
			descriptor_.dimension_z,
			z
		)) {
		return false;
	}
	const std::size_t index =
		(z * descriptor_.dimension_y + y) *
			descriptor_.dimension_x +
		x;
	output.density = densities_[index];
	output.material = materials_.empty() ?
		descriptor_.default_material :
		materials_[index];
	return true;
}

bool WtDenseGridSource::initialized() const noexcept {
	return initialized_;
}

std::size_t WtDenseGridSource::sample_count() const noexcept {
	return densities_.size();
}

const WtDenseGridDescriptor &WtDenseGridSource::descriptor() const noexcept {
	return descriptor_;
}

} // namespace world_transvoxel
