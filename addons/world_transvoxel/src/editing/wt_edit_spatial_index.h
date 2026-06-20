#pragma once

#include "editing/wt_edit_transaction.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace world_transvoxel {

enum class WtEditSpatialStatus : std::uint8_t {
	Ok,
	InvalidInput,
	KeyCapacityExceeded,
	CandidateCapacityExceeded,
	ResultCapacityExceeded,
	DuplicateKey,
};

struct WtEditChunkKeyHash {
	std::size_t operator()(const WtChunkKey &key) const noexcept;
};

class WtEditSpatialIndex {
public:
	WtEditSpatialIndex(
		std::size_t key_capacity,
		std::size_t candidate_capacity,
		std::size_t result_capacity
	);

	WtEditSpatialStatus rebuild(const std::vector<WtChunkKey> &keys);

	WtEditSpatialStatus query_bounds(
		const WtEditBounds &bounds,
		std::vector<WtChunkKey> &output
	) const;

	WtEditSpatialStatus query_transaction(
		const WtEditTransaction &transaction,
		std::vector<WtChunkKey> &output
	) const;

	std::size_t size() const noexcept;
	std::size_t key_capacity() const noexcept;
	std::size_t candidate_capacity() const noexcept;
	std::size_t result_capacity() const noexcept;

private:
	using KeySet = std::unordered_set<WtChunkKey, WtEditChunkKeyHash>;

	WtEditSpatialStatus append_bounds(
		const WtEditBounds &bounds,
		KeySet &results,
		std::size_t &candidate_count
	) const;

	std::size_t key_capacity_ = 0;
	std::size_t candidate_capacity_ = 0;
	std::size_t result_capacity_ = 0;
	KeySet keys_;
	std::array<bool, kWtMaximumLod + 1> populated_lods_{};
};

} // namespace world_transvoxel
