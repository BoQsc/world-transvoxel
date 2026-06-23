#include "wt_dense_file_grid_source.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace world_transvoxel {
namespace {

bool multiply_fits(
	std::size_t left,
	std::size_t right,
	std::size_t &output
) noexcept {
	if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
		return false;
	}
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
	if (delta < 0 || delta % static_cast<__int128_t>(spacing) != 0) {
		return false;
	}
	const __uint128_t index = static_cast<__uint128_t>(delta) / spacing;
	if (index >= dimension) return false;
	output = static_cast<std::size_t>(index);
	return true;
}

bool exact_file_size(
	const std::filesystem::path &path,
	std::size_t expected
) noexcept {
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	return !error && size == expected;
}

bool read_at(
	std::ifstream &input,
	std::size_t offset,
	std::uint8_t *output,
	std::size_t size
) noexcept {
	if (offset > static_cast<std::size_t>(
			std::numeric_limits<std::streamoff>::max()
		) || size > static_cast<std::size_t>(
			std::numeric_limits<std::streamsize>::max()
		)) {
		return false;
	}
	input.clear();
	input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
	input.read(
		reinterpret_cast<char *>(output),
		static_cast<std::streamsize>(size)
	);
	return !input.fail() &&
		static_cast<std::size_t>(input.gcount()) == size;
}

std::uint16_t read_u16(const std::uint8_t *bytes) noexcept {
	return static_cast<std::uint16_t>(bytes[0]) |
		static_cast<std::uint16_t>(
			static_cast<std::uint16_t>(bytes[1]) << 8
		);
}

float read_f32(const std::uint8_t *bytes) noexcept {
	const std::uint32_t bits =
		static_cast<std::uint32_t>(bytes[0]) |
		(static_cast<std::uint32_t>(bytes[1]) << 8) |
		(static_cast<std::uint32_t>(bytes[2]) << 16) |
		(static_cast<std::uint32_t>(bytes[3]) << 24);
	float output = 0.0F;
	std::memcpy(&output, &bits, sizeof(output));
	return output;
}

} // namespace

WtDenseFileGridStatus WtDenseFileGridSource::initialize(
	const WtDenseGridDescriptor &descriptor,
	const std::filesystem::path &density_path,
	const std::filesystem::path *material_path
) {
	initialized_ = false;
	has_materials_ = false;
	sample_count_ = 0;
	descriptor_ = {};
	density_.close();
	materials_.close();
	for (CacheEntry &entry : cache_) {
		entry.row = static_cast<std::size_t>(-1);
		entry.block = static_cast<std::size_t>(-1);
		entry.count = 0;
	}
	if (descriptor.dimension_x == 0 ||
		descriptor.dimension_y == 0 ||
		descriptor.dimension_z == 0 ||
		descriptor.spacing == 0 ||
		descriptor.spacing > static_cast<std::uint64_t>(
			std::numeric_limits<std::int64_t>::max()
		)) {
		return WtDenseFileGridStatus::InvalidInput;
	}
	std::size_t plane = 0;
	std::size_t samples = 0;
	std::size_t density_bytes = 0;
	std::size_t material_bytes = 0;
	if (!multiply_fits(
			descriptor.dimension_x,
			descriptor.dimension_y,
			plane
		) ||
		!multiply_fits(plane, descriptor.dimension_z, samples) ||
		!multiply_fits(samples, sizeof(float), density_bytes) ||
		!multiply_fits(samples, sizeof(std::uint16_t), material_bytes)) {
		return WtDenseFileGridStatus::SizeOverflow;
	}
	if (!exact_file_size(density_path, density_bytes) ||
		(material_path != nullptr &&
			!exact_file_size(*material_path, material_bytes))) {
		return WtDenseFileGridStatus::FileSizeMismatch;
	}
	density_.open(density_path, std::ios::binary);
	if (material_path != nullptr) {
		materials_.open(*material_path, std::ios::binary);
	}
	if (!density_ || (material_path != nullptr && !materials_)) {
		density_.close();
		materials_.close();
		return WtDenseFileGridStatus::FileOpenFailure;
	}
	descriptor_ = descriptor;
	sample_count_ = samples;
	has_materials_ = material_path != nullptr;
	initialized_ = true;
	return WtDenseFileGridStatus::Ok;
}

bool WtDenseFileGridSource::load_block(
	std::size_t row,
	std::size_t block,
	CacheEntry &entry
) const noexcept {
	const std::size_t start = block * kBlockSamples;
	if (start >= descriptor_.dimension_x) return false;
	const std::size_t count = std::min(
		kBlockSamples,
		static_cast<std::size_t>(descriptor_.dimension_x) - start
	);
	const std::size_t sample_offset =
		row * descriptor_.dimension_x + start;
	if (!read_at(
			density_,
			sample_offset * sizeof(float),
			entry.densities.data(),
			count * sizeof(float)
		) ||
		(has_materials_ && !read_at(
			materials_,
			sample_offset * sizeof(std::uint16_t),
			entry.materials.data(),
			count * sizeof(std::uint16_t)
		))) {
		return false;
	}
	entry.row = row;
	entry.block = block;
	entry.count = count;
	return true;
}

bool WtDenseFileGridSource::sample(
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
	const std::size_t row = z * descriptor_.dimension_y + y;
	const std::size_t block = x / kBlockSamples;
	CacheEntry &entry = cache_[
		(row * 1315423911ULL + block) % kCacheSlots
	];
	if ((row != entry.row || block != entry.block) &&
		!load_block(row, block, entry)) {
		return false;
	}
	const std::size_t local = x - block * kBlockSamples;
	if (local >= entry.count) return false;
	output.density = read_f32(
		entry.densities.data() + local * sizeof(float)
	);
	output.material = has_materials_ ?
		read_u16(
			entry.materials.data() +
				local * sizeof(std::uint16_t)
		) :
		descriptor_.default_material;
	return true;
}

bool WtDenseFileGridSource::initialized() const noexcept {
	return initialized_;
}

std::size_t WtDenseFileGridSource::sample_count() const noexcept {
	return sample_count_;
}

std::size_t WtDenseFileGridSource::cache_capacity_bytes() const noexcept {
	return kCacheSlots * kBlockSamples *
		(sizeof(float) + sizeof(std::uint16_t));
}

const WtDenseGridDescriptor &WtDenseFileGridSource::descriptor() const noexcept {
	return descriptor_;
}

} // namespace world_transvoxel
