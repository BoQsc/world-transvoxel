#include "bake/wt_chunk_baker.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace world_transvoxel {

WtChunkBaker::WtChunkBaker(std::size_t page_capacity) noexcept :
		page_capacity_(page_capacity) {
}

WtChunkBakeStatus WtChunkBaker::bake(
	const std::vector<WtChunkKey> &keys,
	std::uint64_t source_revision,
	const WtChunkSampleSource &source,
	std::vector<WtBakedChunkPage> &output
) const {
	output.clear();
	if (keys.size() > page_capacity_) {
		return WtChunkBakeStatus::PageCapacityExceeded;
	}
	std::vector<WtChunkKey> ordered = keys;
	std::sort(ordered.begin(), ordered.end());
	for (std::size_t index = 0; index < ordered.size(); ++index) {
		if (!wt_is_valid_chunk_key(ordered[index])) {
			return WtChunkBakeStatus::InvalidInput;
		}
		if (index != 0 && ordered[index - 1] == ordered[index]) {
			return WtChunkBakeStatus::DuplicateKey;
		}
	}

	output.reserve(ordered.size());
	for (const WtChunkKey &key : ordered) {
		WtChunkPage page;
		page.metadata.key = key;
		page.metadata.cell_spacing = static_cast<std::uint64_t>(
			wt_lod_cell_size(key.lod)
		);
		page.metadata.source_revision = source_revision;
		page.samples.reserve(kWtChunkPageSampleCount);
		const WtChunkBounds bounds = wt_chunk_bounds(key);
		const std::int64_t spacing = wt_lod_cell_size(key.lod);
		for (int z = -1; z <= 17; ++z) {
			for (int y = -1; y <= 17; ++y) {
				for (int x = -1; x <= 17; ++x) {
					const WtGridPoint point = {
						bounds.minimum.x + static_cast<std::int64_t>(x) * spacing,
						bounds.minimum.y + static_cast<std::int64_t>(y) * spacing,
						bounds.minimum.z + static_cast<std::int64_t>(z) * spacing,
					};
					WtScalarSample sample;
					if (!source.sample(point, sample) || !std::isfinite(sample.density)) {
						output.clear();
						return WtChunkBakeStatus::SampleSourceFailure;
					}
					page.samples.push_back(sample);
				}
			}
		}
		WtBakedChunkPage baked;
		baked.key = key;
		if (wt_write_chunk_page(page, baked.bytes) != WtChunkPageStatus::Ok) {
			output.clear();
			return WtChunkBakeStatus::PageWriteFailure;
		}
		baked.content_hash = wt_sha256(baked.bytes.data(), baked.bytes.size());
		output.push_back(std::move(baked));
	}
	return WtChunkBakeStatus::Ok;
}

} // namespace world_transvoxel
