#pragma once

#include "services/wt_publication_spatial_index.h"

#include <memory>

namespace world_transvoxel {

// Thread-owned snapshot of spatial membership, never of readiness, generation,
// density or transition masks. Exact key comparison invalidates each index.
class WtPublicationDependencyGraph {
public:
	void update(const std::vector<WtChunkKey> &replacements, const std::vector<WtChunkKey> &retirements) {
		if (replacement_keys_ != replacements) {
			replacement_keys_ = replacements;
			replacement_index_.reset();
		}
		if (retirement_keys_ != retirements) {
			retirement_keys_ = retirements;
			retirement_index_.reset();
		}
	}
	const WtPublicationSpatialIndex &replacements() {
		if (!replacement_index_) {
			replacement_index_ = std::make_unique<WtPublicationSpatialIndex>(replacement_keys_);
			++index_builds_;
		}
		return *replacement_index_;
	}
	const WtPublicationSpatialIndex &retirements() {
		if (!retirement_index_) {
			retirement_index_ = std::make_unique<WtPublicationSpatialIndex>(retirement_keys_);
			++index_builds_;
		}
		return *retirement_index_;
	}
	std::size_t index_builds() const { return index_builds_; }

private:
	std::vector<WtChunkKey> replacement_keys_, retirement_keys_;
	std::unique_ptr<WtPublicationSpatialIndex> replacement_index_, retirement_index_;
	std::size_t index_builds_ = 0;
};

} // namespace world_transvoxel
