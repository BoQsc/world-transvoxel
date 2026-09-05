#pragma once

#include "editing/wt_edit_transaction.h"
#include "meshing/wt_chunk_mesher.h"
#include "storage/wt_chunk_surface_shift.h"

#include <algorithm>

namespace world_transvoxel {

// Retained corrections are valid only outside every replayed edit's influence.
// The one-unit halo includes the finest-sample gradient stencil. A union of
// command bounds is conservative even for disjoint edits and material changes.
class WtEditSurfaceShiftSource final : public WtChunkSampleSource {
public:
	WtEditSurfaceShiftSource(const WtChunkSampleSource &edited_source,
		const WtChunkPage &retained_page, const WtEditBounds *dirty_bounds) :
		edited_source_(edited_source), retained_page_(retained_page),
		dirty_bounds_(dirty_bounds) {}

	bool sample(const WtGridPoint &point, WtScalarSample &output) const noexcept override {
		return edited_source_.sample(point, output);
	}

	WtMultiresolutionEdgeSourceStatus resolve_multiresolution_edge(
		const WtGridPoint &a, const WtGridPoint &b, float isovalue,
		WtResolvedMultiresolutionEdge &output) const noexcept override {
		if (dirty_bounds_ == nullptr || !retained_page_.surface_shift_valid) {
			return WtMultiresolutionEdgeSourceStatus::Unsupported;
		}
		const auto &dirty = *dirty_bounds_;
		const auto separated = [](std::int64_t lo, std::int64_t hi,
			std::int64_t dirty_lo, std::int64_t dirty_hi) {
			// Unsigned differences avoid overflow at extreme coordinates.
			return (lo > dirty_hi && static_cast<std::uint64_t>(lo) -
				static_cast<std::uint64_t>(dirty_hi) > 1) ||
				(dirty_lo > hi && static_cast<std::uint64_t>(dirty_lo) -
					static_cast<std::uint64_t>(hi) > 1);
		};
		const bool intersects =
			!separated(std::min(a.x, b.x), std::max(a.x, b.x), dirty.minimum.x, dirty.maximum.x) &&
			!separated(std::min(a.y, b.y), std::max(a.y, b.y), dirty.minimum.y, dirty.maximum.y) &&
			!separated(std::min(a.z, b.z), std::max(a.z, b.z), dirty.minimum.z, dirty.maximum.z);
		if (!intersects && wt_resolve_chunk_surface_shift_record(
			retained_page_, a, b, isovalue, output)) {
			return WtMultiresolutionEdgeSourceStatus::Ok;
		}
		return WtMultiresolutionEdgeSourceStatus::Unsupported;
	}

private:
	const WtChunkSampleSource &edited_source_;
	const WtChunkPage &retained_page_;
	const WtEditBounds *dirty_bounds_;
};

} // namespace world_transvoxel
