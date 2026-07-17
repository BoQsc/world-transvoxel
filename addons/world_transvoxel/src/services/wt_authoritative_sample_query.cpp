#include "services/wt_authoritative_sample_query.h"

#include "editing/wt_chunk_edit_state.h"
#include "editing/wt_edit_journal.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_chunk_page.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

namespace world_transvoxel {
namespace {

std::int64_t floor_divide(
	std::int64_t value,
	std::int64_t divisor
) noexcept {
	const std::int64_t quotient = value / divisor;
	const std::int64_t remainder = value % divisor;
	return remainder < 0 ? quotient - 1 : quotient;
}

bool to_i32(std::int64_t value, std::int32_t &output) noexcept {
	if (value < std::numeric_limits<std::int32_t>::min() ||
		value > std::numeric_limits<std::int32_t>::max()) {
		return false;
	}
	output = static_cast<std::int32_t>(value);
	return true;
}

std::vector<WtChunkKey> candidate_keys(
	const WtGridPoint &point,
	std::uint8_t lod
) {
	std::vector<WtChunkKey> keys;
	if (lod > kWtMaximumLod) return keys;
	const std::int64_t spacing = wt_lod_cell_size(lod);
	const std::int64_t extent = wt_chunk_extent(lod);
	if (spacing == 0 || point.x % spacing != 0 ||
		point.y % spacing != 0 || point.z % spacing != 0) {
		return keys;
	}
	const std::int64_t base[3] = {
		floor_divide(point.x, extent),
		floor_divide(point.y, extent),
		floor_divide(point.z, extent),
	};
	keys.reserve(27);
	for (std::int64_t z = base[2] - 1; z <= base[2] + 1; ++z) {
		for (std::int64_t y = base[1] - 1; y <= base[1] + 1; ++y) {
			for (std::int64_t x = base[0] - 1; x <= base[0] + 1; ++x) {
				WtChunkKey key;
				key.lod = lod;
				if (to_i32(x, key.x) && to_i32(y, key.y) &&
					to_i32(z, key.z)) {
					keys.push_back(key);
				}
			}
		}
	}
	std::sort(keys.begin(), keys.end());
	return keys;
}

bool same_sample(
	const WtScalarSample &left,
	const WtScalarSample &right
) noexcept {
	return left.density == right.density &&
		left.material == right.material &&
		left.material_authored == right.material_authored;
}

struct EditedPageCacheEntry {
	WtChunkKey key;
	std::shared_ptr<const WtChunkPage> page;
};

WtAuthoritativeSampleQueryStatus load_edited_page(
	const WtChunkKey &key,
	WtAsyncStorageService &storage,
	const WtEditJournal &journal,
	std::uint64_t initial_world_revision,
	std::vector<EditedPageCacheEntry> &cache,
	const WtChunkPage *&output
) {
	output = nullptr;
	const auto cached = std::find_if(
		cache.begin(),
		cache.end(),
		[&](const EditedPageCacheEntry &entry) { return entry.key == key; }
	);
	if (cached != cache.end()) {
		output = cached->page.get();
		return WtAuthoritativeSampleQueryStatus::Ok;
	}
	if (!storage.has_page(key)) {
		return WtAuthoritativeSampleQueryStatus::Ok;
	}
	std::shared_ptr<const std::vector<std::uint8_t>> bytes;
	if (storage.load_page_now(key, bytes) != WtPageLoadStatus::Ok || !bytes) {
		return WtAuthoritativeSampleQueryStatus::StorageFailure;
	}
	WtChunkPageView view;
	WtChunkPage page;
	if (wt_open_chunk_page(
			{ bytes->data(), bytes->size() },
			view
		) != WtChunkPageStatus::Ok ||
		wt_decode_chunk_page(view, page) != WtChunkPageStatus::Ok) {
		return WtAuthoritativeSampleQueryStatus::PageFailure;
	}
	WtChunkEditState edit_state;
	if (edit_state.initialize(
			std::move(page),
			storage.source_revision(),
			initial_world_revision
		) != WtChunkEditStatus::Ok ||
		journal.replay(edit_state) != WtEditJournalStatus::Ok ||
		edit_state.current_world_revision() != journal.current_world_revision()) {
		return WtAuthoritativeSampleQueryStatus::EditReplayFailure;
	}
	auto edited_page = std::make_shared<WtChunkPage>(edit_state.page());
	output = edited_page.get();
	cache.push_back({ key, std::move(edited_page) });
	return WtAuthoritativeSampleQueryStatus::Ok;
}

} // namespace

WtAuthoritativeSampleQueryStatus wt_query_authoritative_sample(
	const WtGridPoint &point,
	std::uint8_t lod,
	WtAsyncStorageService &storage,
	const WtEditJournal &journal,
	std::uint64_t initial_world_revision,
	WtAuthoritativeSample &output
) {
	output = {};
	const std::vector<WtChunkKey> keys = candidate_keys(point, lod);
	if (keys.empty() || !storage.is_open() || !journal.initialized() ||
		journal.source_revision() != storage.source_revision() ||
		journal.initial_world_revision() != initial_world_revision) {
		return keys.empty() ?
			WtAuthoritativeSampleQueryStatus::InvalidPoint :
			WtAuthoritativeSampleQueryStatus::StorageFailure;
	}
	bool found = false;
	WtScalarSample selected;
	std::size_t agreeing_pages = 0;
	for (const WtChunkKey &key : keys) {
		if (!storage.has_page(key)) continue;
		std::shared_ptr<const std::vector<std::uint8_t>> bytes;
		if (storage.load_page_now(key, bytes) != WtPageLoadStatus::Ok ||
			!bytes) {
			return WtAuthoritativeSampleQueryStatus::StorageFailure;
		}
		WtChunkPageView view;
		WtChunkPage page;
		if (wt_open_chunk_page(
				{ bytes->data(), bytes->size() },
				view
			) != WtChunkPageStatus::Ok ||
			wt_decode_chunk_page(view, page) != WtChunkPageStatus::Ok) {
			return WtAuthoritativeSampleQueryStatus::PageFailure;
		}
		WtChunkEditState edit_state;
		if (edit_state.initialize(
				std::move(page),
				storage.source_revision(),
				initial_world_revision
			) != WtChunkEditStatus::Ok ||
			journal.replay(edit_state) != WtEditJournalStatus::Ok ||
			edit_state.current_world_revision() !=
				journal.current_world_revision()) {
			return WtAuthoritativeSampleQueryStatus::EditReplayFailure;
		}
		WtScalarSample candidate;
		if (!wt_sample_chunk_page(edit_state.page(), point, candidate)) {
			continue;
		}
		if (found && !same_sample(selected, candidate)) {
			return WtAuthoritativeSampleQueryStatus::ConflictingSamples;
		}
		selected = candidate;
		found = true;
		++agreeing_pages;
	}
	if (!found) return WtAuthoritativeSampleQueryStatus::NotFound;
	output.point = point;
	output.lod = lod;
	output.sample = selected;
	output.source_revision = storage.source_revision();
	output.world_revision = journal.current_world_revision();
	output.agreeing_page_count = agreeing_pages;
	return WtAuthoritativeSampleQueryStatus::Ok;
}

WtAuthoritativeSampleQueryStatus wt_query_authoritative_samples(
	const std::vector<WtGridPoint> &points,
	std::uint8_t lod,
	WtAsyncStorageService &storage,
	const WtEditJournal &journal,
	std::uint64_t initial_world_revision,
	std::vector<WtAuthoritativeSample> &output
) {
	output.clear();
	if (points.empty() ||
		points.size() > kWtMaximumAuthoritativeSampleBatchPoints ||
		lod > kWtMaximumLod) {
		return WtAuthoritativeSampleQueryStatus::InvalidPoint;
	}
	if (!storage.is_open() || !journal.initialized() ||
		journal.source_revision() != storage.source_revision() ||
		journal.initial_world_revision() != initial_world_revision) {
		return WtAuthoritativeSampleQueryStatus::StorageFailure;
	}
	std::vector<EditedPageCacheEntry> cache;
	output.reserve(points.size());
	for (const WtGridPoint &point : points) {
		const std::vector<WtChunkKey> keys = candidate_keys(point, lod);
		if (keys.empty()) return WtAuthoritativeSampleQueryStatus::InvalidPoint;
		bool found = false;
		WtScalarSample selected;
		std::size_t agreeing_pages = 0;
		for (const WtChunkKey &key : keys) {
			const WtChunkPage *page = nullptr;
			const WtAuthoritativeSampleQueryStatus status = load_edited_page(
				key,
				storage,
				journal,
				initial_world_revision,
				cache,
				page
			);
			if (status != WtAuthoritativeSampleQueryStatus::Ok) {
				output.clear();
				return status;
			}
			if (page == nullptr) continue;
			WtScalarSample candidate;
			if (!wt_sample_chunk_page(*page, point, candidate)) {
				continue;
			}
			if (found && !same_sample(selected, candidate)) {
				output.clear();
				return WtAuthoritativeSampleQueryStatus::ConflictingSamples;
			}
			selected = candidate;
			found = true;
			++agreeing_pages;
		}
		if (!found) {
			output.clear();
			return WtAuthoritativeSampleQueryStatus::NotFound;
		}
		output.push_back({
			point,
			lod,
			selected,
			storage.source_revision(),
			journal.current_world_revision(),
			agreeing_pages,
		});
	}
	return WtAuthoritativeSampleQueryStatus::Ok;
}

const char *wt_authoritative_sample_query_status_message(
	WtAuthoritativeSampleQueryStatus status
) noexcept {
	switch (status) {
		case WtAuthoritativeSampleQueryStatus::Ok: return "ok";
		case WtAuthoritativeSampleQueryStatus::InvalidPoint:
			return "sample point or LOD is invalid";
		case WtAuthoritativeSampleQueryStatus::NotFound:
			return "sample point is outside indexed world pages";
		case WtAuthoritativeSampleQueryStatus::StorageFailure:
			return "authoritative sample page loading failed";
		case WtAuthoritativeSampleQueryStatus::PageFailure:
			return "authoritative sample page decoding failed";
		case WtAuthoritativeSampleQueryStatus::EditReplayFailure:
			return "authoritative sample edit replay failed";
		case WtAuthoritativeSampleQueryStatus::ConflictingSamples:
			return "overlapping authoritative pages disagree";
	}
	return "unknown authoritative sample query status";
}

} // namespace world_transvoxel
