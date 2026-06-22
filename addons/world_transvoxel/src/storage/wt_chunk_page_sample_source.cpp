#include "storage/wt_chunk_page_sample_source.h"

#include <algorithm>
#include <limits>

namespace world_transvoxel {
namespace {

bool valid_page(const WtChunkPage &page) noexcept {
	return wt_is_valid_chunk_key(page.metadata.key) &&
		page.metadata.sample_minimum == -1 &&
		page.metadata.sample_maximum == 17 &&
		page.metadata.dimension_x == kWtChunkMeshingSamplesPerAxis &&
		page.metadata.dimension_y == kWtChunkMeshingSamplesPerAxis &&
		page.metadata.dimension_z == kWtChunkMeshingSamplesPerAxis &&
		page.metadata.density_encoding == WtDensityEncoding::Float32 &&
		page.metadata.material_encoding == WtMaterialEncoding::Uint16 &&
		page.metadata.sample_count == kWtChunkPageSampleCount &&
		page.metadata.cell_spacing == static_cast<std::uint64_t>(
			wt_lod_cell_size(page.metadata.key.lod)
		) &&
		page.samples.size() == kWtChunkPageSampleCount;
}

bool to_i32(std::int64_t value, std::int32_t &output) noexcept {
	if (value < std::numeric_limits<std::int32_t>::min() ||
		value > std::numeric_limits<std::int32_t>::max()) {
		return false;
	}
	output = static_cast<std::int32_t>(value);
	return true;
}

bool make_support_key(
	const WtChunkKey &coarse_key,
	WtChunkFace face,
	int first,
	int second,
	WtChunkKey &output
) noexcept {
	const std::int64_t child_x =
		static_cast<std::int64_t>(coarse_key.x) * 2;
	const std::int64_t child_y =
		static_cast<std::int64_t>(coarse_key.y) * 2;
	const std::int64_t child_z =
		static_cast<std::int64_t>(coarse_key.z) * 2;
	std::int64_t x = child_x;
	std::int64_t y = child_y;
	std::int64_t z = child_z;
	switch (face) {
		case WtChunkFace::NegativeX:
			x = child_x - 1;
			y += first;
			z += second;
			break;
		case WtChunkFace::PositiveX:
			x = child_x + 2;
			y += first;
			z += second;
			break;
		case WtChunkFace::NegativeY:
			x += first;
			y = child_y - 1;
			z += second;
			break;
		case WtChunkFace::PositiveY:
			x += first;
			y = child_y + 2;
			z += second;
			break;
		case WtChunkFace::NegativeZ:
			x += first;
			y += second;
			z = child_z - 1;
			break;
		case WtChunkFace::PositiveZ:
			x += first;
			y += second;
			z = child_z + 2;
			break;
	}
	return to_i32(x, output.x) &&
		to_i32(y, output.y) &&
		to_i32(z, output.z);
}

bool is_transition_support_key(
	const WtChunkKey &primary,
	const WtChunkKey &candidate
) noexcept {
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		std::array<WtChunkKey, kWtTransitionSupportPagesPerFace> expected{};
		if (!wt_transition_support_page_keys(
				primary,
				static_cast<WtChunkFace>(face_index),
				expected
			)) {
			continue;
		}
		if (std::find(expected.begin(), expected.end(), candidate) !=
			expected.end()) {
			return true;
		}
	}
	return false;
}

bool same_sample(
	const WtScalarSample &left,
	const WtScalarSample &right
) noexcept {
	return left.density == right.density &&
		left.material == right.material;
}

} // namespace

bool wt_transition_support_page_keys(
	const WtChunkKey &coarse_key,
	WtChunkFace face,
	std::array<WtChunkKey, kWtTransitionSupportPagesPerFace> &output
) noexcept {
	output = {};
	if (!wt_is_valid_chunk_key(coarse_key) ||
		coarse_key.lod == 0 ||
		wt_face_bit(face) == 0) {
		return false;
	}
	std::size_t index = 0;
	for (int first = 0; first < 2; ++first) {
		for (int second = 0; second < 2; ++second) {
			WtChunkKey key;
			key.lod = static_cast<std::uint8_t>(coarse_key.lod - 1);
			if (!make_support_key(
					coarse_key,
					face,
					first,
					second,
					key
				)) {
				output = {};
				return false;
			}
			output[index++] = key;
		}
	}
	return true;
}

WtChunkPageSampleSource::WtChunkPageSampleSource(
	const WtChunkPage &primary_page
) noexcept {
	if (valid_page(primary_page)) {
		primary_page_ = &primary_page;
		status_ = WtChunkPageSampleSourceStatus::Ok;
	}
}

WtChunkPageSampleSourceStatus
WtChunkPageSampleSource::add_transition_support_page(
	const WtChunkPage &page
) noexcept {
	if (status_ == WtChunkPageSampleSourceStatus::InvalidPrimaryPage ||
		primary_page_ == nullptr) {
		return status_;
	}
	if (!valid_page(page) ||
		page.metadata.source_revision !=
			primary_page_->metadata.source_revision ||
		!is_transition_support_key(
			primary_page_->metadata.key,
			page.metadata.key
		)) {
		return WtChunkPageSampleSourceStatus::InvalidSupportPage;
	}
	const auto begin = support_pages_.begin();
	const auto end = begin + static_cast<std::ptrdiff_t>(
		support_page_count_
	);
	const auto position = std::lower_bound(
		begin,
		end,
		page.metadata.key,
		[](const WtChunkPage *left, const WtChunkKey &key) {
			return left->metadata.key < key;
		}
	);
	if (position != end && (*position)->metadata.key == page.metadata.key) {
		return WtChunkPageSampleSourceStatus::DuplicateSupportPage;
	}
	if (support_page_count_ == support_pages_.size()) {
		return WtChunkPageSampleSourceStatus::SupportCapacityExceeded;
	}
	std::move_backward(position, end, end + 1);
	*position = &page;
	++support_page_count_;
	return WtChunkPageSampleSourceStatus::Ok;
}

WtChunkPageSampleSourceStatus
WtChunkPageSampleSource::status() const noexcept {
	return status_;
}

std::size_t WtChunkPageSampleSource::support_page_count() const noexcept {
	return support_page_count_;
}

bool WtChunkPageSampleSource::has_transition_support(
	WtChunkFace face
) const noexcept {
	if (primary_page_ == nullptr) {
		return false;
	}
	std::array<WtChunkKey, kWtTransitionSupportPagesPerFace> expected{};
	if (!wt_transition_support_page_keys(
			primary_page_->metadata.key,
			face,
			expected
		)) {
		return false;
	}
	for (const WtChunkKey &key : expected) {
		const auto begin = support_pages_.begin();
		const auto end = begin + static_cast<std::ptrdiff_t>(
			support_page_count_
		);
		const auto found = std::lower_bound(
			begin,
			end,
			key,
			[](const WtChunkPage *left, const WtChunkKey &right) {
				return left->metadata.key < right;
			}
		);
		if (found == end || (*found)->metadata.key != key) {
			return false;
		}
	}
	return true;
}

bool WtChunkPageSampleSource::has_transition_support(
	std::uint8_t transition_mask
) const noexcept {
	if ((transition_mask & 0xc0U) != 0) {
		return false;
	}
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		if ((transition_mask & (1U << face_index)) != 0 &&
			!has_transition_support(
				static_cast<WtChunkFace>(face_index)
			)) {
			return false;
		}
	}
	return true;
}

bool WtChunkPageSampleSource::sample(
	const WtGridPoint &point,
	WtScalarSample &output
) const noexcept {
	if (primary_page_ == nullptr) {
		return false;
	}
	bool found = false;
	WtScalarSample selected;
	for (std::size_t index = 0;
		index <= support_page_count_;
		++index) {
		const WtChunkPage *page = index < support_page_count_ ?
			support_pages_[index] : primary_page_;
		WtScalarSample candidate;
		if (!wt_sample_chunk_page(*page, point, candidate)) {
			continue;
		}
		if (found && !same_sample(selected, candidate)) {
			return false;
		}
		selected = candidate;
		found = true;
	}
	if (found) {
		output = selected;
	}
	return found;
}

} // namespace world_transvoxel
