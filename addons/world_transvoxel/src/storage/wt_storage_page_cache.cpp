#include "storage/wt_storage_page_cache.h"

#include "storage/wt_hash256.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace world_transvoxel {
namespace {

template <typename Entry>
bool entry_identity_less(
	const Entry &entry,
	const std::pair<WtChunkKey, std::uint64_t> &identity
) noexcept {
	if (entry.key != identity.first) {
		return entry.key < identity.first;
	}
	return entry.source_revision < identity.second;
}

template <typename Entry>
bool eviction_precedes(const Entry &left, const Entry &right) noexcept {
	return std::tie(
		left.last_access,
		left.key.x,
		left.key.y,
		left.key.z,
		left.key.lod,
		left.source_revision
	) < std::tie(
		right.last_access,
		right.key.x,
		right.key.y,
		right.key.z,
		right.key.lod,
		right.source_revision
	);
}

std::size_t encoded_resident_size(
	const std::shared_ptr<const std::vector<std::uint8_t>> &bytes
) noexcept {
	return bytes ? sizeof(std::vector<std::uint8_t>) + bytes->capacity() : 0;
}

std::size_t decoded_resident_size(
	const std::shared_ptr<const WtChunkPage> &page
) noexcept {
	return page ? sizeof(WtChunkPage) +
		page->samples.capacity() * sizeof(WtScalarSample) +
		page->surface_shift_records.capacity() *
			sizeof(WtChunkSurfaceShiftRecord) : 0;
}

} // namespace

WtStoragePageCache::WtStoragePageCache(WtStoragePageCacheLimits limits) :
		limits_(limits) {
	valid_ =
		limits_.encoded_entry_capacity != 0 &&
		limits_.encoded_entry_capacity <= kWtMaximumStorageCacheEntries &&
		limits_.encoded_byte_capacity >= kWtContainerHeaderSize &&
		limits_.encoded_byte_capacity <= kWtMaximumStorageCacheBytes &&
		limits_.decoded_entry_capacity != 0 &&
		limits_.decoded_entry_capacity <= kWtMaximumStorageCacheEntries &&
		limits_.decoded_byte_capacity >= sizeof(WtChunkPage) &&
		limits_.decoded_byte_capacity <= kWtMaximumStorageCacheBytes;
	if (valid_) {
		encoded_.reserve(limits_.encoded_entry_capacity);
		decoded_.reserve(limits_.decoded_entry_capacity);
	}
}

bool WtStoragePageCache::valid() const noexcept {
	return valid_;
}

WtStoragePageCacheStatus WtStoragePageCache::accept_completion(
	const WtPageLoadCompletion &completion,
	WtGenerationToken current_generation
) {
	if (!valid_) {
		return WtStoragePageCacheStatus::InvalidConfiguration;
	}
	if (!wt_is_valid_chunk_key(completion.key) ||
		completion.generation.value == 0 ||
		current_generation.value == 0) {
		return WtStoragePageCacheStatus::InvalidInput;
	}
	if (completion.generation != current_generation) {
		++metrics_.stale_completions;
		return WtStoragePageCacheStatus::StaleGeneration;
	}
	if (completion.status != WtPageLoadStatus::Ok ||
		!completion.page_bytes) {
		++metrics_.load_failures;
		return WtStoragePageCacheStatus::LoadFailure;
	}

	const std::shared_ptr<const std::vector<std::uint8_t>> &bytes =
		completion.page_bytes;
	WtChunkPageView view;
	if (wt_open_chunk_page(
			{ bytes->data(), bytes->size() },
			view
		) != WtChunkPageStatus::Ok ||
		view.metadata.key != completion.key) {
		++metrics_.invalid_pages;
		return WtStoragePageCacheStatus::InvalidPage;
	}
	const WtHash256 content_hash =
		wt_sha256(bytes->data(), bytes->size());
	const std::size_t resident_bytes = encoded_resident_size(bytes);
	if (resident_bytes > limits_.encoded_byte_capacity) {
		++metrics_.encoded_oversize_rejections;
		return WtStoragePageCacheStatus::EncodedItemTooLarge;
	}

	auto decoded_identity = find_decoded_entry(
		completion.key,
		view.metadata.source_revision
	);
	if (decoded_identity != decoded_.end() &&
		decoded_identity->content_hash != content_hash) {
		++metrics_.identity_conflicts;
		return WtStoragePageCacheStatus::IdentityConflict;
	}
	auto existing = find_encoded_entry(
		completion.key,
		view.metadata.source_revision
	);
	if (existing != encoded_.end()) {
		if (existing->content_hash != content_hash) {
			++metrics_.identity_conflicts;
			return WtStoragePageCacheStatus::IdentityConflict;
		}
		encoded_resident_bytes_ -= existing->resident_bytes;
		existing->bytes = bytes;
		existing->resident_bytes = resident_bytes;
		existing->last_access = next_access();
		encoded_resident_bytes_ += resident_bytes;
		++metrics_.encoded_refreshes;
		evict_encoded_to_limits();
		++metrics_.accepted_completions;
		return WtStoragePageCacheStatus::Ok;
	}

	EncodedEntry entry;
	entry.key = completion.key;
	entry.source_revision = view.metadata.source_revision;
	entry.content_hash = content_hash;
	entry.bytes = bytes;
	entry.resident_bytes = resident_bytes;
	entry.last_access = next_access();
	const auto position = std::lower_bound(
		encoded_.begin(),
		encoded_.end(),
		std::make_pair(entry.key, entry.source_revision),
		entry_identity_less<EncodedEntry>
	);
	encoded_resident_bytes_ += resident_bytes;
	encoded_.insert(position, std::move(entry));
	++metrics_.encoded_insertions;
	++metrics_.accepted_completions;
	evict_encoded_to_limits();
	return WtStoragePageCacheStatus::Ok;
}

std::shared_ptr<const std::vector<std::uint8_t>>
WtStoragePageCache::find_encoded(
	const WtChunkKey &key,
	std::uint64_t source_revision
) {
	if (!valid_ || !wt_is_valid_chunk_key(key)) {
		++metrics_.encoded_misses;
		return {};
	}
	auto entry = find_encoded_entry(key, source_revision);
	if (entry == encoded_.end()) {
		++metrics_.encoded_misses;
		return {};
	}
	entry->last_access = next_access();
	++metrics_.encoded_hits;
	return entry->bytes;
}

WtStoragePageCacheStatus WtStoragePageCache::find_or_decode(
	const WtChunkKey &key,
	std::uint64_t source_revision,
	std::shared_ptr<const WtChunkPage> &page
) {
	page.reset();
	if (!valid_) {
		return WtStoragePageCacheStatus::InvalidConfiguration;
	}
	if (!wt_is_valid_chunk_key(key)) {
		return WtStoragePageCacheStatus::InvalidInput;
	}
	auto decoded = find_decoded_entry(key, source_revision);
	if (decoded != decoded_.end()) {
		decoded->last_access = next_access();
		page = decoded->page;
		++metrics_.decoded_hits;
		return WtStoragePageCacheStatus::Ok;
	}
	++metrics_.decoded_misses;

	auto encoded = find_encoded_entry(key, source_revision);
	if (encoded == encoded_.end()) {
		++metrics_.encoded_misses;
		return WtStoragePageCacheStatus::NotFound;
	}
	encoded->last_access = next_access();
	++metrics_.encoded_hits;
	const std::size_t minimum_decoded_bytes =
		sizeof(WtChunkPage) +
		kWtChunkPageSampleCount * sizeof(WtScalarSample);
	if (minimum_decoded_bytes > limits_.decoded_byte_capacity) {
		++metrics_.decoded_oversize_rejections;
		return WtStoragePageCacheStatus::DecodedItemTooLarge;
	}
	WtChunkPageView view;
	if (wt_open_chunk_page(
			{ encoded->bytes->data(), encoded->bytes->size() },
			view
		) != WtChunkPageStatus::Ok) {
		++metrics_.decode_failures;
		return WtStoragePageCacheStatus::InvalidPage;
	}
	auto mutable_page = std::make_shared<WtChunkPage>();
	if (wt_decode_chunk_page(view, *mutable_page) != WtChunkPageStatus::Ok) {
		++metrics_.decode_failures;
		return WtStoragePageCacheStatus::InvalidPage;
	}
	const std::size_t resident_bytes = decoded_resident_size(mutable_page);
	if (resident_bytes > limits_.decoded_byte_capacity) {
		++metrics_.decoded_oversize_rejections;
		return WtStoragePageCacheStatus::DecodedItemTooLarge;
	}

	DecodedEntry entry;
	entry.key = key;
	entry.source_revision = source_revision;
	entry.content_hash = encoded->content_hash;
	entry.page = std::move(mutable_page);
	entry.resident_bytes = resident_bytes;
	entry.last_access = next_access();
	const auto position = std::lower_bound(
		decoded_.begin(),
		decoded_.end(),
		std::make_pair(entry.key, entry.source_revision),
		entry_identity_less<DecodedEntry>
	);
	decoded_resident_bytes_ += resident_bytes;
	const auto inserted = decoded_.insert(position, std::move(entry));
	page = inserted->page;
	++metrics_.decoded_insertions;
	evict_decoded_to_limits();
	return WtStoragePageCacheStatus::Ok;
}

std::size_t WtStoragePageCache::erase_key(const WtChunkKey &key) {
	std::size_t erased = 0;
	for (auto iterator = encoded_.begin(); iterator != encoded_.end();) {
		if (iterator->key == key) {
			encoded_resident_bytes_ -= iterator->resident_bytes;
			iterator = encoded_.erase(iterator);
			++erased;
		} else {
			++iterator;
		}
	}
	for (auto iterator = decoded_.begin(); iterator != decoded_.end();) {
		if (iterator->key == key) {
			decoded_resident_bytes_ -= iterator->resident_bytes;
			iterator = decoded_.erase(iterator);
			++erased;
		} else {
			++iterator;
		}
	}
	return erased;
}

void WtStoragePageCache::clear() noexcept {
	encoded_.clear();
	decoded_.clear();
	encoded_resident_bytes_ = 0;
	decoded_resident_bytes_ = 0;
}

std::uint64_t WtStoragePageCache::next_access() noexcept {
	if (access_counter_ == std::numeric_limits<std::uint64_t>::max()) {
		std::uint64_t minimum = access_counter_;
		for (EncodedEntry &entry : encoded_) {
			minimum = std::min(minimum, entry.last_access);
		}
		for (DecodedEntry &entry : decoded_) {
			minimum = std::min(minimum, entry.last_access);
		}
		if (encoded_.empty() && decoded_.empty()) {
			access_counter_ = 0;
		} else {
			const std::uint64_t offset = minimum - 1;
			for (EncodedEntry &entry : encoded_) {
				entry.last_access -= offset;
			}
			for (DecodedEntry &entry : decoded_) {
				entry.last_access -= offset;
			}
			access_counter_ -= offset;
		}
	}
	return ++access_counter_;
}

std::vector<WtStoragePageCache::EncodedEntry>::iterator
WtStoragePageCache::find_encoded_entry(
	const WtChunkKey &key,
	std::uint64_t source_revision
) {
	const auto identity = std::make_pair(key, source_revision);
	const auto iterator = std::lower_bound(
		encoded_.begin(),
		encoded_.end(),
		identity,
		entry_identity_less<EncodedEntry>
	);
	return iterator != encoded_.end() &&
		iterator->key == key &&
		iterator->source_revision == source_revision ?
		iterator : encoded_.end();
}

std::vector<WtStoragePageCache::DecodedEntry>::iterator
WtStoragePageCache::find_decoded_entry(
	const WtChunkKey &key,
	std::uint64_t source_revision
) {
	const auto identity = std::make_pair(key, source_revision);
	const auto iterator = std::lower_bound(
		decoded_.begin(),
		decoded_.end(),
		identity,
		entry_identity_less<DecodedEntry>
	);
	return iterator != decoded_.end() &&
		iterator->key == key &&
		iterator->source_revision == source_revision ?
		iterator : decoded_.end();
}

void WtStoragePageCache::evict_encoded_to_limits() {
	while (encoded_.size() > limits_.encoded_entry_capacity ||
		encoded_resident_bytes_ > limits_.encoded_byte_capacity) {
		const auto victim = std::min_element(
			encoded_.begin(),
			encoded_.end(),
			eviction_precedes<EncodedEntry>
		);
		encoded_resident_bytes_ -= victim->resident_bytes;
		encoded_.erase(victim);
		++metrics_.encoded_evictions;
	}
}

void WtStoragePageCache::evict_decoded_to_limits() {
	while (decoded_.size() > limits_.decoded_entry_capacity ||
		decoded_resident_bytes_ > limits_.decoded_byte_capacity) {
		const auto victim = std::min_element(
			decoded_.begin(),
			decoded_.end(),
			eviction_precedes<DecodedEntry>
		);
		decoded_resident_bytes_ -= victim->resident_bytes;
		decoded_.erase(victim);
		++metrics_.decoded_evictions;
	}
}

std::size_t WtStoragePageCache::encoded_entry_count() const noexcept {
	return encoded_.size();
}

std::size_t WtStoragePageCache::encoded_resident_bytes() const noexcept {
	return encoded_resident_bytes_;
}

std::size_t WtStoragePageCache::decoded_entry_count() const noexcept {
	return decoded_.size();
}

std::size_t WtStoragePageCache::decoded_resident_bytes() const noexcept {
	return decoded_resident_bytes_;
}

WtStoragePageCacheMetrics WtStoragePageCache::get_metrics() const noexcept {
	return metrics_;
}

} // namespace world_transvoxel
