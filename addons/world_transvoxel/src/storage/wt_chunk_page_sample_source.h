#pragma once

#include "storage/wt_chunk_page.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace world_transvoxel {

constexpr std::size_t kWtTransitionSupportPagesPerFace = 4;
constexpr std::size_t kWtMaximumTransitionSupportPages =
	kWtTransitionSupportPagesPerFace * 6;

enum class WtChunkPageSampleSourceStatus : std::uint8_t {
	Ok,
	InvalidPrimaryPage,
	InvalidSupportPage,
	DuplicateSupportPage,
	SupportCapacityExceeded,
};

bool wt_transition_support_page_keys(
	const WtChunkKey &coarse_key,
	WtChunkFace face,
	std::array<WtChunkKey, kWtTransitionSupportPagesPerFace> &output
) noexcept;

class WtChunkPageSampleSource final : public WtChunkSampleSource {
public:
	explicit WtChunkPageSampleSource(
		const WtChunkPage &primary_page
	) noexcept;

	WtChunkPageSampleSourceStatus add_transition_support_page(
		const WtChunkPage &page
	) noexcept;

	WtChunkPageSampleSourceStatus status() const noexcept;
	std::size_t support_page_count() const noexcept;
	bool has_transition_support(WtChunkFace face) const noexcept;
	bool has_transition_support(std::uint8_t transition_mask) const noexcept;

	bool sample(
		const WtGridPoint &point,
		WtScalarSample &output
	) const noexcept override;

private:
	const WtChunkPage *primary_page_ = nullptr;
	std::array<
		const WtChunkPage *,
		kWtMaximumTransitionSupportPages
	> support_pages_{};
	std::size_t support_page_count_ = 0;
	WtChunkPageSampleSourceStatus status_ =
		WtChunkPageSampleSourceStatus::InvalidPrimaryPage;
};

} // namespace world_transvoxel
