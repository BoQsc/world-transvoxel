#pragma once

#include "storage/wt_async_storage_service.h"
#include "storage/wt_chunk_page.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace world_transvoxel {

constexpr std::size_t kWtMaximumStorageCacheEntries = 65536;
constexpr std::size_t kWtMaximumStorageCacheBytes =
	1024U * 1024U * 1024U;

struct WtStoragePageCacheLimits {
	std::size_t encoded_entry_capacity = 256;
	std::size_t encoded_byte_capacity = 64U * 1024U * 1024U;
	std::size_t decoded_entry_capacity = 128;
	std::size_t decoded_byte_capacity = 64U * 1024U * 1024U;
};

enum class WtStoragePageCacheStatus : std::uint8_t {
	Ok,
	InvalidConfiguration,
	InvalidInput,
	StaleGeneration,
	LoadFailure,
	InvalidPage,
	IdentityConflict,
	EncodedItemTooLarge,
	DecodedItemTooLarge,
	NotFound,
};

struct WtStoragePageCacheMetrics {
	std::uint64_t accepted_completions = 0;
	std::uint64_t stale_completions = 0;
	std::uint64_t load_failures = 0;
	std::uint64_t invalid_pages = 0;
	std::uint64_t identity_conflicts = 0;
	std::uint64_t encoded_hits = 0;
	std::uint64_t encoded_misses = 0;
	std::uint64_t encoded_insertions = 0;
	std::uint64_t encoded_refreshes = 0;
	std::uint64_t encoded_evictions = 0;
	std::uint64_t encoded_oversize_rejections = 0;
	std::uint64_t decoded_hits = 0;
	std::uint64_t decoded_misses = 0;
	std::uint64_t decoded_insertions = 0;
	std::uint64_t decoded_evictions = 0;
	std::uint64_t decoded_oversize_rejections = 0;
	std::uint64_t decode_failures = 0;
};

class WtStoragePageCache {
public:
	explicit WtStoragePageCache(WtStoragePageCacheLimits limits);

	bool valid() const noexcept;
	WtStoragePageCacheStatus accept_completion(
		const WtPageLoadCompletion &completion,
		WtGenerationToken current_generation
	);
	std::shared_ptr<const std::vector<std::uint8_t>> find_encoded(
		const WtChunkKey &key,
		std::uint64_t source_revision
	);
	WtStoragePageCacheStatus find_or_decode(
		const WtChunkKey &key,
		std::uint64_t source_revision,
		std::shared_ptr<const WtChunkPage> &page
	);
	std::size_t erase_key(const WtChunkKey &key);
	void clear() noexcept;

	std::size_t encoded_entry_count() const noexcept;
	std::size_t encoded_resident_bytes() const noexcept;
	std::size_t decoded_entry_count() const noexcept;
	std::size_t decoded_resident_bytes() const noexcept;
	WtStoragePageCacheMetrics get_metrics() const noexcept;

private:
	struct EncodedEntry {
		WtChunkKey key;
		std::uint64_t source_revision = 0;
		WtHash256 content_hash{};
		std::shared_ptr<const std::vector<std::uint8_t>> bytes;
		std::size_t resident_bytes = 0;
		std::uint64_t last_access = 0;
	};

	struct DecodedEntry {
		WtChunkKey key;
		std::uint64_t source_revision = 0;
		WtHash256 content_hash{};
		std::shared_ptr<const WtChunkPage> page;
		std::size_t resident_bytes = 0;
		std::uint64_t last_access = 0;
	};

	std::uint64_t next_access() noexcept;
	std::vector<EncodedEntry>::iterator find_encoded_entry(
		const WtChunkKey &key,
		std::uint64_t source_revision
	);
	std::vector<DecodedEntry>::iterator find_decoded_entry(
		const WtChunkKey &key,
		std::uint64_t source_revision
	);
	void evict_encoded_to_limits();
	void evict_decoded_to_limits();

	WtStoragePageCacheLimits limits_;
	bool valid_ = false;
	std::uint64_t access_counter_ = 0;
	std::size_t encoded_resident_bytes_ = 0;
	std::size_t decoded_resident_bytes_ = 0;
	std::vector<EncodedEntry> encoded_;
	std::vector<DecodedEntry> decoded_;
	WtStoragePageCacheMetrics metrics_;
};

} // namespace world_transvoxel
