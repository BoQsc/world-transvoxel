#include "bake/wt_chunk_baker.h"
#include "storage/wt_hash256.h"
#include "storage/wt_storage_page_cache.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace wt = world_transvoxel;

namespace {

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

class CacheSource final : public wt::WtChunkSampleSource {
public:
	explicit CacheSource(float offset) : offset_(offset) {
	}

	bool sample(
		const wt::WtGridPoint &point,
		wt::WtScalarSample &output
	) const noexcept override {
		output.density =
			static_cast<float>(point.x + point.y - point.z) * 0.0625F +
			offset_;
		output.material = static_cast<std::uint16_t>(
			(static_cast<std::uint64_t>(point.x) * 11U) ^
			(static_cast<std::uint64_t>(point.y) * 13U) ^
			(static_cast<std::uint64_t>(point.z) * 17U)
		);
		return true;
	}

private:
	float offset_ = 0.0F;
};

std::vector<wt::WtBakedChunkPage> bake_pages(
	const std::vector<wt::WtChunkKey> &keys,
	std::uint64_t source_revision,
	float offset
) {
	const CacheSource source(offset);
	wt::WtChunkBaker baker(keys.size());
	std::vector<wt::WtBakedChunkPage> pages;
	check(
		baker.bake(keys, source_revision, source, pages) ==
			wt::WtChunkBakeStatus::Ok,
		"cache fixture bake failed"
	);
	return pages;
}

wt::WtPageLoadCompletion completion(
	const wt::WtBakedChunkPage &page,
	std::uint64_t generation
) {
	wt::WtPageLoadCompletion result;
	result.key = page.key;
	result.generation = { generation };
	result.status = wt::WtPageLoadStatus::Ok;
	result.page_bytes =
		std::make_shared<const std::vector<std::uint8_t>>(page.bytes);
	return result;
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
	for (std::size_t index = 0; index < 8; ++index) {
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
	}
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void test_invalid_limits(const wt::WtBakedChunkPage &page) {
	wt::WtStoragePageCache invalid({
		0,
		wt::kWtMaximumContainerSize,
		1,
		wt::kWtMaximumContainerSize,
	});
	check(!invalid.valid(), "zero encoded capacity was accepted");
	check(
		invalid.accept_completion(completion(page, 1), { 1 }) ==
			wt::WtStoragePageCacheStatus::InvalidConfiguration,
		"invalid cache accepted a completion"
	);

	wt::WtStoragePageCache encoded_small({
		1,
		wt::kWtContainerHeaderSize,
		1,
		wt::kWtMaximumContainerSize,
	});
	check(
		encoded_small.accept_completion(completion(page, 1), { 1 }) ==
			wt::WtStoragePageCacheStatus::EncodedItemTooLarge,
		"oversize encoded page was accepted"
	);

	wt::WtStoragePageCache decoded_small({
		1,
		wt::kWtMaximumContainerSize,
		1,
		sizeof(wt::WtChunkPage),
	});
	check(
		decoded_small.accept_completion(completion(page, 1), { 1 }) ==
			wt::WtStoragePageCacheStatus::Ok,
		"decoded oversize fixture insert failed"
	);
	std::shared_ptr<const wt::WtChunkPage> decoded;
	check(
		decoded_small.find_or_decode(page.key, 500, decoded) ==
			wt::WtStoragePageCacheStatus::DecodedItemTooLarge &&
			!decoded,
		"oversize decoded page was accepted"
	);
}

void test_rejection_paths(
	wt::WtStoragePageCache &cache,
	const std::vector<wt::WtBakedChunkPage> &pages
) {
	check(
		cache.accept_completion(completion(pages[0], 1), { 2 }) ==
			wt::WtStoragePageCacheStatus::StaleGeneration,
		"stale completion was cached"
	);
	wt::WtPageLoadCompletion failed = completion(pages[0], 2);
	failed.status = wt::WtPageLoadStatus::HashMismatch;
	failed.page_bytes.reset();
	check(
		cache.accept_completion(failed, { 2 }) ==
			wt::WtStoragePageCacheStatus::LoadFailure,
		"failed load completion was cached"
	);
	wt::WtPageLoadCompletion mismatched = completion(pages[0], 3);
	mismatched.key = pages[1].key;
	check(
		cache.accept_completion(mismatched, { 3 }) ==
			wt::WtStoragePageCacheStatus::InvalidPage,
		"mismatched page metadata was cached"
	);
	check(
		cache.accept_completion(completion(pages[0], 0), { 1 }) ==
			wt::WtStoragePageCacheStatus::InvalidInput,
		"zero generation was accepted"
	);
}

void test_lru_and_ownership(
	const std::vector<wt::WtBakedChunkPage> &pages,
	std::vector<std::uint8_t> &evidence
) {
	wt::WtStoragePageCache cache({
		2,
		wt::kWtMaximumContainerSize,
		2,
		wt::kWtMaximumContainerSize,
	});
	check(cache.valid(), "valid cache configuration was rejected");
	test_rejection_paths(cache, pages);

	check(
		cache.accept_completion(completion(pages[0], 10), { 10 }) ==
			wt::WtStoragePageCacheStatus::Ok &&
		cache.accept_completion(completion(pages[1], 11), { 11 }) ==
			wt::WtStoragePageCacheStatus::Ok,
		"initial cache insertion failed"
	);
	check(
		cache.accept_completion(completion(pages[0], 12), { 12 }) ==
			wt::WtStoragePageCacheStatus::Ok,
		"immutable identity refresh failed"
	);
	check(
		cache.find_encoded(pages[0].key, 500) != nullptr,
		"encoded refresh lookup failed"
	);
	check(
		cache.accept_completion(completion(pages[2], 13), { 13 }) ==
			wt::WtStoragePageCacheStatus::Ok,
		"third encoded insertion failed"
	);
	check(
		cache.find_encoded(pages[1].key, 500) == nullptr &&
			cache.find_encoded(pages[0].key, 500) != nullptr &&
			cache.find_encoded(pages[2].key, 500) != nullptr,
		"encoded LRU eviction selected the wrong entry"
	);

	std::shared_ptr<const wt::WtChunkPage> decoded0;
	std::shared_ptr<const wt::WtChunkPage> decoded0_hit;
	std::shared_ptr<const wt::WtChunkPage> decoded2;
	check(
		cache.find_or_decode(pages[0].key, 500, decoded0) ==
			wt::WtStoragePageCacheStatus::Ok &&
		cache.find_or_decode(pages[0].key, 500, decoded0_hit) ==
			wt::WtStoragePageCacheStatus::Ok &&
		decoded0 == decoded0_hit,
		"decoded cache hit did not preserve immutable ownership"
	);
	check(
		cache.find_or_decode(pages[2].key, 500, decoded2) ==
			wt::WtStoragePageCacheStatus::Ok,
		"second decoded insertion failed"
	);
	const float retained_density = decoded0->samples[0].density;
	check(
		cache.accept_completion(completion(pages[3], 14), { 14 }) ==
			wt::WtStoragePageCacheStatus::Ok,
		"fourth encoded insertion failed"
	);
	std::shared_ptr<const wt::WtChunkPage> decoded3;
	check(
		cache.find_or_decode(pages[3].key, 500, decoded3) ==
			wt::WtStoragePageCacheStatus::Ok,
		"third decoded insertion failed"
	);
	std::shared_ptr<const wt::WtChunkPage> evicted;
	check(
		cache.find_or_decode(pages[0].key, 500, evicted) ==
			wt::WtStoragePageCacheStatus::NotFound &&
			!evicted,
		"decoded LRU entry remained reachable through the cache"
	);
	check(
		decoded0->samples[0].density == retained_density,
		"external immutable handle did not survive cache eviction"
	);
	check(
		cache.erase_key(pages[2].key) == 2,
		"explicit key eviction did not remove both tiers"
	);
	check(
		cache.encoded_entry_count() == 1 &&
			cache.decoded_entry_count() == 1,
		"cache counts mismatch after explicit eviction"
	);

	const wt::WtStoragePageCacheMetrics metrics = cache.get_metrics();
	check(
		metrics.accepted_completions == 5 &&
			metrics.stale_completions == 1 &&
			metrics.load_failures == 1 &&
			metrics.invalid_pages == 1,
		"completion metrics mismatch"
	);
	check(
		metrics.encoded_insertions == 4 &&
			metrics.encoded_refreshes == 1 &&
			metrics.encoded_evictions == 2,
		"encoded cache metrics mismatch"
	);
	check(
		metrics.decoded_insertions == 3 &&
			metrics.decoded_hits == 1 &&
			metrics.decoded_evictions == 1,
		"decoded cache metrics mismatch"
	);

	evidence = pages[0].bytes;
	append_u64(evidence, metrics.accepted_completions);
	append_u64(evidence, metrics.encoded_hits);
	append_u64(evidence, metrics.encoded_misses);
	append_u64(evidence, metrics.encoded_evictions);
	append_u64(evidence, metrics.decoded_hits);
	append_u64(evidence, metrics.decoded_misses);
	append_u64(evidence, metrics.decoded_evictions);
	append_u64(evidence, cache.encoded_resident_bytes());
	append_u64(evidence, cache.decoded_resident_bytes());
	cache.clear();
	check(
		cache.encoded_entry_count() == 0 &&
			cache.decoded_entry_count() == 0 &&
			cache.encoded_resident_bytes() == 0 &&
			cache.decoded_resident_bytes() == 0,
		"cache clear retained residency"
	);
}

void test_identity_conflict(const wt::WtBakedChunkPage &page) {
	const std::vector<wt::WtBakedChunkPage> alternate = bake_pages(
		{ page.key },
		500,
		4.0F
	);
	const std::vector<wt::WtBakedChunkPage> other = bake_pages(
		{ { page.key.x + 1, page.key.y, page.key.z, page.key.lod } },
		500,
		0.0F
	);
	wt::WtStoragePageCache cache({
		1,
		wt::kWtMaximumContainerSize,
		2,
		wt::kWtMaximumContainerSize,
	});
	std::shared_ptr<const wt::WtChunkPage> decoded;
	check(
		cache.accept_completion(completion(page, 30), { 30 }) ==
			wt::WtStoragePageCacheStatus::Ok &&
		cache.find_or_decode(page.key, 500, decoded) ==
			wt::WtStoragePageCacheStatus::Ok &&
		cache.accept_completion(completion(other[0], 31), { 31 }) ==
			wt::WtStoragePageCacheStatus::Ok &&
		cache.find_encoded(page.key, 500) == nullptr &&
		decoded != nullptr,
		"identity conflict fixture did not retain decoded-only ownership"
	);
	check(
		cache.accept_completion(completion(alternate[0], 31), { 31 }) ==
			wt::WtStoragePageCacheStatus::IdentityConflict,
		"cross-tier conflicting immutable identity was accepted"
	);
	check(
		cache.get_metrics().identity_conflicts == 1 &&
			cache.encoded_entry_count() == 1 &&
			cache.decoded_entry_count() == 1,
		"identity conflict mutated cache residency"
	);
}

} // namespace

int main() {
	const std::vector<wt::WtChunkKey> keys = {
		{ 0, 0, 0, 0 },
		{ 1, 0, 0, 0 },
		{ 2, 0, 0, 0 },
		{ 3, 0, 0, 0 },
	};
	const std::vector<wt::WtBakedChunkPage> pages =
		bake_pages(keys, 500, 0.0F);
	if (pages.size() != keys.size()) {
		std::fprintf(stderr, "M5_STORAGE_CACHE_FAIL fixture_pages=%zu\n",
			pages.size());
		return 1;
	}
	test_invalid_limits(pages[0]);
	std::vector<std::uint8_t> evidence;
	test_lru_and_ownership(pages, evidence);
	test_identity_conflict(pages[0]);
	if (failure_count != 0) {
		std::fprintf(stderr, "M5_STORAGE_CACHE_FAIL failures=%d\n",
			failure_count);
		return 1;
	}
	std::printf("M5_STORAGE_CACHE_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"M5_STORAGE_CACHE_PASS encoded_capacity=2 decoded_capacity=2 "
		"stale=1 identity_conflicts=1\n"
	);
	return 0;
}
