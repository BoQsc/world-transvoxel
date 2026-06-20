#include "editing/wt_edit_spatial_index.h"

#include "core/wt_meshing_limits.h"

#include <algorithm>
#include <limits>
#include <set>

namespace world_transvoxel {
namespace {

std::uint64_t mix64(std::uint64_t value) noexcept {
	value ^= value >> 30;
	value *= 0xbf58476d1ce4e5b9ULL;
	value ^= value >> 27;
	value *= 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

std::int64_t floor_divide(
	std::int64_t numerator,
	std::int64_t denominator
) noexcept {
	std::int64_t quotient = numerator / denominator;
	if (numerator % denominator < 0) {
		--quotient;
	}
	return quotient;
}

std::int64_t ceil_divide(
	std::int64_t numerator,
	std::int64_t denominator
) noexcept {
	std::int64_t quotient = numerator / denominator;
	if (numerator % denominator > 0) {
		++quotient;
	}
	return quotient;
}

bool valid_bounds(const WtEditBounds &bounds) noexcept {
	return bounds.minimum.x <= bounds.maximum.x &&
		bounds.minimum.y <= bounds.maximum.y &&
		bounds.minimum.z <= bounds.maximum.z;
}

bool add_fits(
	std::size_t left,
	std::size_t right,
	std::size_t maximum
) noexcept {
	return left <= maximum && right <= maximum - left;
}

bool multiply_fits(
	std::size_t left,
	std::size_t right,
	std::size_t maximum,
	std::size_t &output
) noexcept {
	if (left != 0 && right > maximum / left) {
		return false;
	}
	output = left * right;
	return true;
}

bool axis_range(
	std::int64_t minimum,
	std::int64_t maximum,
	std::int64_t spacing,
	std::int64_t extent,
	std::int32_t &first,
	std::int32_t &last,
	std::size_t &count
) noexcept {
	const std::int64_t lower_padding = extent + spacing;
	const std::int64_t minimum_numerator =
		minimum < std::numeric_limits<std::int64_t>::min() + lower_padding ?
			std::numeric_limits<std::int64_t>::min() :
			minimum - lower_padding;
	const std::int64_t maximum_numerator =
		maximum > std::numeric_limits<std::int64_t>::max() - spacing ?
			std::numeric_limits<std::int64_t>::max() :
			maximum + spacing;
	std::int64_t first64 = ceil_divide(minimum_numerator, extent);
	std::int64_t last64 = floor_divide(maximum_numerator, extent);
	first64 = std::max<std::int64_t>(
		first64,
		std::numeric_limits<std::int32_t>::min()
	);
	last64 = std::min<std::int64_t>(
		last64,
		std::numeric_limits<std::int32_t>::max()
	);
	if (first64 > last64) {
		count = 0;
		return true;
	}
	first = static_cast<std::int32_t>(first64);
	last = static_cast<std::int32_t>(last64);
	count = static_cast<std::size_t>(
		static_cast<std::uint64_t>(last64 - first64) + 1
	);
	return true;
}

} // namespace

std::size_t WtEditChunkKeyHash::operator()(
	const WtChunkKey &key
) const noexcept {
	const std::uint64_t x = static_cast<std::uint32_t>(key.x);
	const std::uint64_t y = static_cast<std::uint32_t>(key.y);
	const std::uint64_t z = static_cast<std::uint32_t>(key.z);
	const std::uint64_t lod = key.lod;
	return static_cast<std::size_t>(
		mix64(x | (lod << 32)) ^
		mix64(y + 0x9e3779b97f4a7c15ULL) ^
		mix64(z + 0xd1b54a32d192ed03ULL)
	);
}

WtEditSpatialIndex::WtEditSpatialIndex(
	std::size_t key_capacity,
	std::size_t candidate_capacity,
	std::size_t result_capacity
) :
		key_capacity_(key_capacity),
		candidate_capacity_(candidate_capacity),
		result_capacity_(result_capacity) {
	keys_.reserve(key_capacity_);
}

WtEditSpatialStatus WtEditSpatialIndex::rebuild(
	const std::vector<WtChunkKey> &keys
) {
	if (keys.size() > key_capacity_) {
		return WtEditSpatialStatus::KeyCapacityExceeded;
	}
	KeySet replacement;
	replacement.reserve(key_capacity_);
	std::array<bool, kWtMaximumLod + 1> populated_lods{};
	for (const WtChunkKey &key : keys) {
		if (!wt_is_valid_chunk_key(key)) {
			return WtEditSpatialStatus::InvalidInput;
		}
		if (!replacement.insert(key).second) {
			return WtEditSpatialStatus::DuplicateKey;
		}
		populated_lods[key.lod] = true;
	}
	keys_.swap(replacement);
	populated_lods_ = populated_lods;
	return WtEditSpatialStatus::Ok;
}

WtEditSpatialStatus WtEditSpatialIndex::append_bounds(
	const WtEditBounds &bounds,
	KeySet &results,
	std::size_t &candidate_count
) const {
	if (!valid_bounds(bounds)) {
		return WtEditSpatialStatus::InvalidInput;
	}
	for (std::uint8_t lod = 0; lod <= kWtMaximumLod; ++lod) {
		if (!populated_lods_[lod]) {
			continue;
		}
		const std::int64_t spacing = wt_lod_cell_size(lod);
		const std::int64_t extent = wt_chunk_extent(lod);
		std::int32_t first[3]{};
		std::int32_t last[3]{};
		std::size_t count[3]{};
		if (!axis_range(
				bounds.minimum.x,
				bounds.maximum.x,
				spacing,
				extent,
				first[0],
				last[0],
				count[0]
			) ||
			!axis_range(
				bounds.minimum.y,
				bounds.maximum.y,
				spacing,
				extent,
				first[1],
				last[1],
				count[1]
			) ||
			!axis_range(
				bounds.minimum.z,
				bounds.maximum.z,
				spacing,
				extent,
				first[2],
				last[2],
				count[2]
			)) {
			return WtEditSpatialStatus::InvalidInput;
		}
		std::size_t plane_count = 0;
		std::size_t lod_count = 0;
		if (!multiply_fits(
				count[0],
				count[1],
				candidate_capacity_,
				plane_count
			) ||
			!multiply_fits(
				plane_count,
				count[2],
				candidate_capacity_,
				lod_count
			) ||
			!add_fits(
				candidate_count,
				lod_count,
				candidate_capacity_
			)) {
			return WtEditSpatialStatus::CandidateCapacityExceeded;
		}
		candidate_count += lod_count;
		for (std::int64_t z = first[2]; z <= last[2]; ++z) {
			for (std::int64_t y = first[1]; y <= last[1]; ++y) {
				for (std::int64_t x = first[0]; x <= last[0]; ++x) {
					const WtChunkKey key = {
						static_cast<std::int32_t>(x),
						static_cast<std::int32_t>(y),
						static_cast<std::int32_t>(z),
						lod,
					};
					if (keys_.find(key) != keys_.end()) {
						if (results.size() >= result_capacity_ &&
							results.find(key) == results.end()) {
							return WtEditSpatialStatus::ResultCapacityExceeded;
						}
						results.insert(key);
					}
				}
			}
		}
	}
	return WtEditSpatialStatus::Ok;
}

WtEditSpatialStatus WtEditSpatialIndex::query_bounds(
	const WtEditBounds &bounds,
	std::vector<WtChunkKey> &output
) const {
	output.clear();
	KeySet results;
	results.reserve(std::min(result_capacity_, keys_.size()));
	std::size_t candidate_count = 0;
	const WtEditSpatialStatus status =
		append_bounds(bounds, results, candidate_count);
	if (status != WtEditSpatialStatus::Ok) {
		return status;
	}
	output.assign(results.begin(), results.end());
	std::sort(output.begin(), output.end());
	return WtEditSpatialStatus::Ok;
}

WtEditSpatialStatus WtEditSpatialIndex::query_transaction(
	const WtEditTransaction &transaction,
	std::vector<WtChunkKey> &output
) const {
	output.clear();
	if (wt_is_zero_id(transaction.transaction_id) ||
		transaction.base_revision == std::numeric_limits<std::uint64_t>::max() ||
		transaction.committed_revision != transaction.base_revision + 1 ||
		transaction.commands.empty() ||
		transaction.commands.size() > kWtMaximumEditCommandCount) {
		return WtEditSpatialStatus::InvalidInput;
	}
	KeySet results;
	results.reserve(std::min(result_capacity_, keys_.size()));
	std::size_t candidate_count = 0;
	std::vector<bool> sequences(transaction.commands.size(), false);
	std::set<WtId128> command_ids;
	for (const WtEditCommand &command : transaction.commands) {
		if (!wt_is_valid_edit_command(command) ||
			command.world_revision != transaction.committed_revision ||
			command.sequence >= transaction.commands.size() ||
			sequences[command.sequence] ||
			!command_ids.insert(command.command_id).second) {
			return WtEditSpatialStatus::InvalidInput;
		}
		sequences[command.sequence] = true;
		const WtEditSpatialStatus status =
			append_bounds(command.bounds, results, candidate_count);
		if (status != WtEditSpatialStatus::Ok) {
			return status;
		}
	}
	output.assign(results.begin(), results.end());
	std::sort(output.begin(), output.end());
	return WtEditSpatialStatus::Ok;
}

std::size_t WtEditSpatialIndex::size() const noexcept {
	return keys_.size();
}

std::size_t WtEditSpatialIndex::key_capacity() const noexcept {
	return key_capacity_;
}

std::size_t WtEditSpatialIndex::candidate_capacity() const noexcept {
	return candidate_capacity_;
}

std::size_t WtEditSpatialIndex::result_capacity() const noexcept {
	return result_capacity_;
}

} // namespace world_transvoxel
