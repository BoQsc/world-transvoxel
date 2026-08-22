#include "storage/wt_world_snapshot_store.h"

#include "bake/wt_chunk_baker.h"
#include "bake/wt_snapshot_compactor.h"
#include "editing/wt_chunk_edit_state.h"
#include "editing/wt_edit_journal.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_chunk_page.h"
#include "storage/wt_hash256.h"
#include "storage/wt_procedural_snapshot_descriptor.h"
#include "storage/wt_procedural_world_source.h"
#include "storage/wt_world_manifest.h"
#include "storage/wt_world_snapshot_store_io.h"
#include "testing/wt_fault_injection.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace world_transvoxel {
namespace {

WtHash256 hash_text(const char *text) {
	const std::string value(text);
	return wt_sha256(
		reinterpret_cast<const std::uint8_t *>(value.data()),
		value.size()
	);
}

std::vector<WtDependencyEntry> procedural_dependencies(
	const WtHash256 &configuration_hash,
	const WtHash256 *previous_world_hash,
	const WtHash256 *journal_hash
) {
	std::vector<WtDependencyEntry> dependencies = {
		{ WtDependencyKind::SourceAsset, "procedural/base", "1",
			configuration_hash },
		{ WtDependencyKind::Generator, "world-transvoxel-procedural", "1",
			hash_text("world-transvoxel-procedural-v1") },
		{ WtDependencyKind::Configuration, "procedural-world", "1",
			configuration_hash },
		{ WtDependencyKind::Backend, "transvoxel-mit", "authority",
			hash_text("transvoxel-mit-authority") },
		{ WtDependencyKind::Godot, "godot", "runtime",
			hash_text("godot-runtime") },
		{ WtDependencyKind::GodotCpp, "godot-cpp", "runtime",
			hash_text("godot-cpp-runtime") },
		{ WtDependencyKind::Toolchain, "native-toolchain", "runtime",
			hash_text("native-toolchain-runtime") },
	};
	if (previous_world_hash != nullptr) {
		dependencies.push_back({
			WtDependencyKind::SourceAsset,
			kWtPreviousWorldAuditLabel,
			"",
			*previous_world_hash,
		});
	}
	if (journal_hash != nullptr) {
		dependencies.push_back({
			WtDependencyKind::SourceAsset,
			kWtEditJournalAuditLabel,
			"",
			*journal_hash,
		});
	}
	return dependencies;
}

std::int64_t floor_divide(
	std::int64_t numerator,
	std::int64_t denominator
) noexcept {
	std::int64_t quotient = numerator / denominator;
	if (numerator % denominator < 0) --quotient;
	return quotient;
}

std::int64_t saturated_add(
	std::int64_t value,
	std::int64_t delta
) noexcept {
	if (delta > 0 && value > std::numeric_limits<std::int64_t>::max() - delta) {
		return std::numeric_limits<std::int64_t>::max();
	}
	if (delta < 0 && value < std::numeric_limits<std::int64_t>::min() - delta) {
		return std::numeric_limits<std::int64_t>::min();
	}
	return value + delta;
}

bool page_intersects_edit(
	const WtChunkKey &key,
	const WtEditBounds &bounds
) noexcept {
	const WtChunkBounds chunk = wt_chunk_bounds(key);
	const std::int64_t spacing = wt_lod_cell_size(key.lod);
	return bounds.maximum.x >= chunk.minimum.x - spacing &&
		bounds.minimum.x <= chunk.maximum.x + spacing &&
		bounds.maximum.y >= chunk.minimum.y - spacing &&
		bounds.minimum.y <= chunk.maximum.y + spacing &&
		bounds.maximum.z >= chunk.minimum.z - spacing &&
		bounds.minimum.z <= chunk.maximum.z + spacing;
}

class CommandCollector final : public WtEditReplaySink {
public:
	explicit CommandCollector(std::size_t capacity) {
		commands.reserve(capacity);
	}

	bool apply(const WtEditCommand &command) noexcept override {
		if (commands.size() >= commands.capacity()) return false;
		commands.push_back(command);
		return true;
	}

	std::vector<WtEditCommand> commands;
};

bool append_affected_keys(
	const WtProceduralWorldDescriptor &descriptor,
	const WtEditCommand &command,
	std::set<WtChunkKey> &keys
) {
	for (std::uint8_t lod = 0; lod <= kWtProceduralMaximumLod; ++lod) {
		WtChunkKey declared_minimum;
		WtChunkKey declared_maximum;
		if (!wt_procedural_lod_key_bounds(
				descriptor, lod, declared_minimum, declared_maximum
			)) return false;
		const std::int64_t spacing = wt_lod_cell_size(lod);
		const std::int64_t extent = wt_chunk_extent(lod);
		const std::int64_t available_minimum[3] = {
			declared_minimum.x, declared_minimum.y, declared_minimum.z,
		};
		const std::int64_t available_maximum[3] = {
			declared_maximum.x, declared_maximum.y, declared_maximum.z,
		};
		const std::int64_t edit_minimum[3] = {
			command.bounds.minimum.x,
			command.bounds.minimum.y,
			command.bounds.minimum.z,
		};
		const std::int64_t edit_maximum[3] = {
			command.bounds.maximum.x,
			command.bounds.maximum.y,
			command.bounds.maximum.z,
		};
		std::int64_t minimum[3]{};
		std::int64_t maximum[3]{};
		for (std::size_t axis = 0; axis < 3; ++axis) {
			minimum[axis] = std::max(
				available_minimum[axis],
				saturated_add(
					floor_divide(
						saturated_add(edit_minimum[axis], -spacing), extent
					),
					-1
				)
			);
			maximum[axis] = std::min(
				available_maximum[axis],
				saturated_add(
					floor_divide(
						saturated_add(edit_maximum[axis], spacing), extent
					),
					1
				)
			);
		}
		if (minimum[0] > maximum[0] || minimum[1] > maximum[1] ||
			minimum[2] > maximum[2]) continue;
		for (std::int64_t z = minimum[2]; z <= maximum[2]; ++z) {
			for (std::int64_t y = minimum[1]; y <= maximum[1]; ++y) {
				for (std::int64_t x = minimum[0]; x <= maximum[0]; ++x) {
					const WtChunkKey key {
						static_cast<std::int32_t>(x),
						static_cast<std::int32_t>(y),
						static_cast<std::int32_t>(z),
						lod,
					};
					if (!page_intersects_edit(key, command.bounds)) continue;
					keys.insert(key);
					if (keys.size() > kWtMaximumProceduralOverlayPageCount) {
						return false;
					}
				}
			}
		}
	}
	return true;
}

class PointEditReplaySink final : public WtEditReplaySink {
public:
	PointEditReplaySink(
		const WtGridPoint &point,
		WtScalarSample &sample,
		const WtProceduralWorldDescriptor *procedural_descriptor
	) noexcept :
			point_(point),
			sample_(sample),
			procedural_descriptor_(procedural_descriptor) {
	}

	bool apply(const WtEditCommand &command) noexcept override {
		bool changed = false;
		return wt_apply_edit_command_to_sample(
			command, point_, sample_, changed, procedural_descriptor_
		);
	}

private:
	const WtGridPoint &point_;
	WtScalarSample &sample_;
	const WtProceduralWorldDescriptor *procedural_descriptor_ = nullptr;
};

class ProceduralCompactionLod0Source final : public WtChunkSampleSource {
public:
	ProceduralCompactionLod0Source(
		WtAsyncStorageService &storage,
		const WtEditJournal &journal,
		std::uint64_t initial_world_revision
	) noexcept :
			storage_(storage),
			journal_(journal),
			initial_world_revision_(initial_world_revision) {
		valid_ = storage_.procedural_descriptor(procedural_descriptor_);
		cache_.reserve(kCacheCapacity);
	}

	bool sample(
		const WtGridPoint &point,
		WtScalarSample &output
	) const noexcept override {
		const std::int64_t base[3] = {
			floor_divide(point.x, kWtChunkCellsPerAxis),
			floor_divide(point.y, kWtChunkCellsPerAxis),
			floor_divide(point.z, kWtChunkCellsPerAxis),
		};
		std::vector<WtChunkKey> candidates;
		candidates.reserve(27);
		for (std::int64_t z = base[2] - 1; z <= base[2] + 1; ++z) {
			for (std::int64_t y = base[1] - 1; y <= base[1] + 1; ++y) {
				for (std::int64_t x = base[0] - 1; x <= base[0] + 1; ++x) {
					if (x < std::numeric_limits<std::int32_t>::min() ||
						x > std::numeric_limits<std::int32_t>::max() ||
						y < std::numeric_limits<std::int32_t>::min() ||
						y > std::numeric_limits<std::int32_t>::max() ||
						z < std::numeric_limits<std::int32_t>::min() ||
						z > std::numeric_limits<std::int32_t>::max()) continue;
					candidates.push_back({
						static_cast<std::int32_t>(x),
						static_cast<std::int32_t>(y),
						static_cast<std::int32_t>(z),
						0,
					});
				}
			}
		}
		std::sort(candidates.begin(), candidates.end());
		for (const WtChunkKey &key : candidates) {
			if (!storage_.has_overlay_page(key)) continue;
			const WtChunkPage *page = find_or_load(key);
			if (page != nullptr && wt_sample_chunk_page(*page, point, output)) {
				return true;
			}
		}
		if (!valid_ || !storage_.sample_procedural_base(point, output)) {
			return false;
		}
		PointEditReplaySink sink(point, output, &procedural_descriptor_);
		return journal_.replay(sink) == WtEditJournalStatus::Ok;
	}

private:
	struct CacheEntry {
		WtChunkKey key;
		WtChunkPage page;
	};

	const WtChunkPage *find_or_load(const WtChunkKey &key) const {
		const auto cached = std::find_if(
			cache_.begin(), cache_.end(),
			[&](const CacheEntry &entry) { return entry.key == key; }
		);
		if (cached != cache_.end()) return &cached->page;
		std::shared_ptr<const std::vector<std::uint8_t>> bytes;
		if (storage_.load_page_now(key, bytes) != WtPageLoadStatus::Ok ||
			!bytes) return nullptr;
		WtChunkPageView view;
		WtChunkPage page;
		if (wt_open_chunk_page(
				{ bytes->data(), bytes->size() }, view
			) != WtChunkPageStatus::Ok ||
			wt_decode_chunk_page(view, page) != WtChunkPageStatus::Ok) {
			return nullptr;
		}
		WtChunkEditState edit_state;
		if (edit_state.initialize(
				std::move(page),
				storage_.source_revision(),
				initial_world_revision_,
				&procedural_descriptor_
			) != WtChunkEditStatus::Ok ||
			journal_.replay(edit_state) != WtEditJournalStatus::Ok ||
			edit_state.current_world_revision() !=
				journal_.current_world_revision()) {
			return nullptr;
		}
		if (cache_.size() >= kCacheCapacity) cache_.erase(cache_.begin());
		cache_.push_back({ key, edit_state.page() });
		return &cache_.back().page;
	}

	static constexpr std::size_t kCacheCapacity = 64;
	WtAsyncStorageService &storage_;
	const WtEditJournal &journal_;
	std::uint64_t initial_world_revision_ = 0;
	WtProceduralWorldDescriptor procedural_descriptor_;
	bool valid_ = false;
	mutable std::vector<CacheEntry> cache_;
};

WtWorldSnapshotStoreStatus read_source(
	WtAsyncStorageService &storage,
	std::vector<std::uint8_t> &world_bytes,
	WtWorldManifestView &world,
	std::vector<WtBakedChunkPage> &pages
) {
	if (!storage.snapshot_manifest(world_bytes) ||
		wt_open_world_manifest(
			{ world_bytes.data(), world_bytes.size() },
			world
		) != WtWorldManifestStatus::Ok) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	if (world.pages.size() > kWtProductionSnapshotPageCapacity) {
		return WtWorldSnapshotStoreStatus::CapacityExceeded;
	}
	std::size_t source_bytes = 0;
	for (const WtWorldPageIndexEntry &entry : world.pages) {
		if (entry.byte_size >
			kWtProductionSnapshotSourceByteCapacity - source_bytes) {
			return WtWorldSnapshotStoreStatus::CapacityExceeded;
		}
		source_bytes += static_cast<std::size_t>(entry.byte_size);
	}
	pages.clear();
	pages.reserve(world.pages.size());
	for (const WtWorldPageIndexEntry &entry : world.pages) {
		std::shared_ptr<const std::vector<std::uint8_t>> bytes;
		if (storage.load_page_now(entry.key, bytes) != WtPageLoadStatus::Ok ||
			!bytes) {
			return WtWorldSnapshotStoreStatus::PageFailure;
		}
		WtBakedChunkPage page;
		page.key = entry.key;
		page.content_hash = entry.content_hash;
		page.bytes = *bytes;
		pages.push_back(std::move(page));
	}
	return WtWorldSnapshotStoreStatus::Ok;
}

WtWorldSnapshotStoreStatus publish(
	const std::filesystem::path &output_directory,
	const std::vector<std::uint8_t> &world_bytes,
	const std::vector<WtBakedChunkPage> &pages,
	WtWorldSnapshotStoreResult &output
) {
	output = {};
	if (output_directory.empty() || output_directory.filename().empty()) {
		return WtWorldSnapshotStoreStatus::InvalidInput;
	}
	std::error_code error;
	const std::filesystem::path parent = output_directory.parent_path().empty() ?
		std::filesystem::current_path(error) :
		output_directory.parent_path();
	if (error || !std::filesystem::is_directory(parent, error) || error) {
		return WtWorldSnapshotStoreStatus::InvalidInput;
	}
	if (std::filesystem::exists(output_directory, error) || error) {
		return error ? WtWorldSnapshotStoreStatus::IoFailure :
			WtWorldSnapshotStoreStatus::OutputExists;
	}
	WtWorldManifestView view;
	if (wt_open_world_manifest(
			{ world_bytes.data(), world_bytes.size() },
			view
		) != WtWorldManifestStatus::Ok ||
		view.pages.size() != pages.size()) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	const std::filesystem::path temporary =
		parent / (output_directory.filename().string() + ".tmp");
	if (std::filesystem::exists(temporary, error) || error ||
		!std::filesystem::create_directory(temporary, error) || error) {
		return WtWorldSnapshotStoreStatus::IoFailure;
	}
	bool wrote_all = wt_snapshot_write_file(
		temporary / "world.wtworld", world_bytes
	);
	for (const WtBakedChunkPage &page : pages) {
		wrote_all = wrote_all && wt_snapshot_write_file(
			temporary / (wt_snapshot_hash_hex(page.content_hash) + ".wtchunk"),
			page.bytes
		);
	}
	if (wrote_all) wrote_all = wt_snapshot_sync_directory(temporary);
	if (!wrote_all) {
		std::filesystem::remove_all(temporary, error);
		return WtWorldSnapshotStoreStatus::IoFailure;
	}
	std::filesystem::rename(temporary, output_directory, error);
	if (error) {
		std::filesystem::remove_all(temporary, error);
		return WtWorldSnapshotStoreStatus::PublishFailure;
	}
	if (!wt_snapshot_sync_directory(parent)) {
		std::filesystem::remove_all(output_directory, error);
		return WtWorldSnapshotStoreStatus::PublishFailure;
	}
	output.manifest_path = output_directory / "world.wtworld";
	output.source_revision = view.source_revision;
	output.world_revision = view.world_revision;
	output.page_count = view.pages.size();
	return WtWorldSnapshotStoreStatus::Ok;
}

WtWorldSnapshotStoreStatus publish_procedural(
	const std::filesystem::path &output_directory,
	const std::vector<std::uint8_t> &descriptor_bytes,
	const std::vector<std::uint8_t> &world_bytes,
	const std::vector<WtBakedChunkPage> &pages,
	WtWorldSnapshotStoreResult &output
) {
	output = {};
	if (output_directory.empty() || output_directory.filename().empty()) {
		return WtWorldSnapshotStoreStatus::InvalidInput;
	}
	WtProceduralSnapshotDescriptor descriptor;
	WtWorldManifestView world;
	if (wt_open_procedural_snapshot_descriptor(
			{ descriptor_bytes.data(), descriptor_bytes.size() }, descriptor
		) != WtProceduralSnapshotDescriptorStatus::Ok ||
		wt_open_world_manifest(
			{ world_bytes.data(), world_bytes.size() }, world
		) != WtWorldManifestStatus::Ok ||
		wt_sha256(world_bytes.data(), world_bytes.size()) !=
			descriptor.overlay_manifest_hash ||
		world.source_revision != descriptor.world.source_revision ||
		world.world_revision != descriptor.world.world_revision ||
		world.configuration_hash !=
			wt_procedural_configuration_hash(descriptor.world) ||
		world.pages.size() != pages.size()) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	for (std::size_t index = 0; index < pages.size(); ++index) {
		if (world.pages[index].key != pages[index].key ||
			pages[index].content_hash != wt_sha256(
				pages[index].bytes.data(), pages[index].bytes.size()
			) ||
			wt_validate_world_page(
				world,
				pages[index].key,
				{ pages[index].bytes.data(), pages[index].bytes.size() }
			) != WtWorldPageStatus::Ok) {
			return WtWorldSnapshotStoreStatus::PageFailure;
		}
	}
	std::error_code error;
	const std::filesystem::path parent = output_directory.parent_path().empty() ?
		std::filesystem::current_path(error) : output_directory.parent_path();
	if (error || !std::filesystem::is_directory(parent, error) || error) {
		return WtWorldSnapshotStoreStatus::InvalidInput;
	}
	if (std::filesystem::exists(output_directory, error) || error) {
		return error ? WtWorldSnapshotStoreStatus::IoFailure :
			WtWorldSnapshotStoreStatus::OutputExists;
	}
	const std::filesystem::path temporary =
		parent / (output_directory.filename().string() + ".tmp");
	if (std::filesystem::exists(temporary, error) || error ||
		!std::filesystem::create_directory(temporary, error) || error) {
		return WtWorldSnapshotStoreStatus::IoFailure;
	}
	bool wrote_all = wt_snapshot_write_file(
		temporary / "world.wtworld", world_bytes
	);
	for (const WtBakedChunkPage &page : pages) {
		wrote_all = wrote_all && wt_snapshot_write_file(
			temporary / (wt_snapshot_hash_hex(page.content_hash) + ".wtchunk"),
			page.bytes
		);
	}
	// Write the entry-point descriptor only after staging its complete overlay.
	wrote_all = wrote_all && wt_snapshot_write_file(
		temporary / "world.wtproc", descriptor_bytes
	);
	if (wrote_all) wrote_all = wt_snapshot_sync_directory(temporary);
	if (!wrote_all) {
		std::filesystem::remove_all(temporary, error);
		return WtWorldSnapshotStoreStatus::IoFailure;
	}
	std::filesystem::rename(temporary, output_directory, error);
	if (error) {
		std::filesystem::remove_all(temporary, error);
		return WtWorldSnapshotStoreStatus::PublishFailure;
	}
	if (!wt_snapshot_sync_directory(parent)) {
		std::filesystem::remove_all(output_directory, error);
		return WtWorldSnapshotStoreStatus::PublishFailure;
	}
	output.manifest_path = output_directory / "world.wtproc";
	output.source_revision = descriptor.world.source_revision;
	output.world_revision = descriptor.world.world_revision;
	output.page_count = pages.size();
	return WtWorldSnapshotStoreStatus::Ok;
}

WtWorldSnapshotStoreStatus read_existing_overlay(
	WtAsyncStorageService &storage,
	std::vector<std::uint8_t> &world_bytes,
	WtWorldManifestView &world,
	std::set<WtChunkKey> &keys
) {
	world_bytes.clear();
	world = {};
	if (!storage.snapshot_manifest(world_bytes)) return WtWorldSnapshotStoreStatus::Ok;
	if (wt_open_world_manifest(
			{ world_bytes.data(), world_bytes.size() }, world
		) != WtWorldManifestStatus::Ok ||
		world.pages.size() > kWtMaximumProceduralOverlayPageCount) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	for (const WtWorldPageIndexEntry &entry : world.pages) keys.insert(entry.key);
	return WtWorldSnapshotStoreStatus::Ok;
}

WtWorldSnapshotStoreStatus encode_overlay_pages(
	WtAsyncStorageService &storage,
	const WtEditJournal &journal,
	const std::set<WtChunkKey> &keys,
	std::uint64_t new_source_revision,
	bool apply_journal,
	std::vector<WtBakedChunkPage> &pages
) {
	pages.clear();
	pages.reserve(keys.size());
	std::size_t byte_count = 0;
	ProceduralCompactionLod0Source lod0_source(
		storage, journal, storage.world_revision()
	);
	WtProceduralWorldDescriptor procedural_descriptor;
	if (!storage.procedural_descriptor(procedural_descriptor)) {
		return WtWorldSnapshotStoreStatus::InvalidInput;
	}
	WtMultiresolutionVertexScratch surface_shift_scratch;
	for (const WtChunkKey &key : keys) {
		std::shared_ptr<const std::vector<std::uint8_t>> source_bytes;
		if (storage.load_page_now(key, source_bytes) != WtPageLoadStatus::Ok ||
			!source_bytes) {
			return WtWorldSnapshotStoreStatus::PageFailure;
		}
		WtChunkPageView view;
		WtChunkPage page;
		if (wt_open_chunk_page(
				{ source_bytes->data(), source_bytes->size() }, view
			) != WtChunkPageStatus::Ok ||
			wt_decode_chunk_page(view, page) != WtChunkPageStatus::Ok) {
			return WtWorldSnapshotStoreStatus::PageFailure;
		}
		if (apply_journal) {
			WtChunkEditState edit_state;
			if (edit_state.initialize(
					std::move(page),
					storage.source_revision(),
					storage.world_revision(),
					&procedural_descriptor
				) != WtChunkEditStatus::Ok ||
				journal.replay(edit_state) != WtEditJournalStatus::Ok ||
				edit_state.current_world_revision() !=
					journal.current_world_revision()) {
				return WtWorldSnapshotStoreStatus::CompactionFailure;
			}
			page = edit_state.page();
		}
		if (!page.surface_shift_valid &&
			wt_build_surface_shift_records(
				page, lod0_source, surface_shift_scratch
			) != WtSurfaceShiftBuildStatus::Ok) {
			return WtWorldSnapshotStoreStatus::CompactionFailure;
		}
		page.metadata.source_revision = new_source_revision;
		WtBakedChunkPage baked;
		baked.key = key;
		if (wt_write_chunk_page(page, baked.bytes) != WtChunkPageStatus::Ok) {
			return WtWorldSnapshotStoreStatus::CompactionFailure;
		}
		if (baked.bytes.size() >
			kWtProductionSnapshotSourceByteCapacity - byte_count) {
			return WtWorldSnapshotStoreStatus::CapacityExceeded;
		}
		byte_count += baked.bytes.size();
		baked.content_hash = wt_sha256(baked.bytes.data(), baked.bytes.size());
		pages.push_back(std::move(baked));
	}
	return WtWorldSnapshotStoreStatus::Ok;
}

WtWorldSnapshotStoreStatus write_procedural_snapshot(
	WtAsyncStorageService &storage,
	const WtEditJournal &journal,
	const std::filesystem::path &output_directory,
	std::uint64_t new_source_revision,
	bool compact,
	WtWorldSnapshotStoreResult &output
) {
	WtProceduralWorldDescriptor source_descriptor;
	if (!storage.procedural_descriptor(source_descriptor)) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	std::vector<std::uint8_t> previous_world_bytes;
	WtWorldManifestView previous_world;
	std::set<WtChunkKey> keys;
	const WtWorldSnapshotStoreStatus previous_status = read_existing_overlay(
		storage, previous_world_bytes, previous_world, keys
	);
	if (previous_status != WtWorldSnapshotStoreStatus::Ok) {
		return previous_status;
	}
	if (compact) {
		CommandCollector collector(journal.command_count());
		if (journal.replay(collector) != WtEditJournalStatus::Ok ||
			collector.commands.size() != journal.command_count()) {
			return WtWorldSnapshotStoreStatus::CompactionFailure;
		}
		for (const WtEditCommand &command : collector.commands) {
			if (!append_affected_keys(source_descriptor, command, keys)) {
				return WtWorldSnapshotStoreStatus::CapacityExceeded;
			}
		}
	}
	if (keys.size() > kWtMaximumProceduralOverlayPageCount) {
		return WtWorldSnapshotStoreStatus::CapacityExceeded;
	}
	std::vector<WtBakedChunkPage> pages;
	const WtWorldSnapshotStoreStatus page_status = encode_overlay_pages(
		storage,
		journal,
		keys,
		new_source_revision,
		compact,
		pages
	);
	if (page_status != WtWorldSnapshotStoreStatus::Ok) return page_status;

	WtProceduralWorldDescriptor output_descriptor = source_descriptor;
	output_descriptor.source_revision = new_source_revision;
	output_descriptor.world_revision = compact ?
		journal.current_world_revision() : source_descriptor.world_revision;
	const WtHash256 configuration_hash =
		wt_procedural_configuration_hash(output_descriptor);
	WtHash256 previous_hash = previous_world_bytes.empty() ?
		configuration_hash :
		wt_sha256(previous_world_bytes.data(), previous_world_bytes.size());
	std::vector<std::uint8_t> journal_bytes;
	WtHash256 journal_hash{};
	if (compact && (
			journal.save(journal_bytes) != WtEditJournalStatus::Ok
		)) {
		return WtWorldSnapshotStoreStatus::CompactionFailure;
	}
	if (compact) {
		journal_hash = wt_sha256(journal_bytes.data(), journal_bytes.size());
	}
	WtWorldManifest manifest;
	manifest.source_revision = output_descriptor.source_revision;
	manifest.world_revision = output_descriptor.world_revision;
	manifest.configuration_hash = configuration_hash;
	manifest.dependencies = procedural_dependencies(
		configuration_hash,
		compact ? &previous_hash : nullptr,
		compact ? &journal_hash : nullptr
	);
	for (const WtBakedChunkPage &page : pages) {
		manifest.pages.push_back({
			page.key,
			page.bytes.size(),
			page.content_hash,
		});
	}
	std::vector<std::uint8_t> world_bytes;
	const WtWorldManifestStatus manifest_status =
		wt_write_world_manifest(manifest, world_bytes);
	if (manifest_status != WtWorldManifestStatus::Ok) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	WtProceduralSnapshotDescriptor snapshot_descriptor;
	snapshot_descriptor.world = output_descriptor;
	snapshot_descriptor.overlay_manifest_hash = wt_sha256(
		world_bytes.data(), world_bytes.size()
	);
	std::vector<std::uint8_t> descriptor_bytes;
	const WtProceduralSnapshotDescriptorStatus descriptor_status =
		wt_write_procedural_snapshot_descriptor(
			snapshot_descriptor, descriptor_bytes
		);
	if (descriptor_status != WtProceduralSnapshotDescriptorStatus::Ok) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	const WtWorldSnapshotStoreStatus publish_status = publish_procedural(
		output_directory,
		descriptor_bytes,
		world_bytes,
		pages,
		output
	);
	return publish_status;
}

} // namespace

WtWorldSnapshotStoreStatus wt_write_migrated_world_snapshot(
	WtAsyncStorageService &storage,
	const WtEditJournal &journal,
	const std::filesystem::path &output_directory,
	WtWorldSnapshotStoreResult &output
) {
	output = {};
	if (!journal.initialized() ||
		journal.source_revision() != storage.source_revision() ||
		journal.initial_world_revision() != storage.world_revision()) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	if (journal.transaction_count() != 0) {
		return WtWorldSnapshotStoreStatus::JournalNotEmpty;
	}
	if (wt_should_inject_fault(
			WtFaultInjectionSite::SnapshotWorkspaceAllocation
		)) {
		return WtWorldSnapshotStoreStatus::AllocationFailure;
	}
	if (storage.is_procedural()) {
		return write_procedural_snapshot(
			storage,
			journal,
			output_directory,
			storage.source_revision(),
			false,
			output
		);
	}
	std::vector<std::uint8_t> previous_world;
	WtWorldManifestView view;
	std::vector<WtBakedChunkPage> pages;
	const WtWorldSnapshotStoreStatus read = read_source(
		storage, previous_world, view, pages
	);
	if (read != WtWorldSnapshotStoreStatus::Ok) return read;
	WtWorldManifest manifest;
	manifest.source_revision = view.source_revision;
	manifest.world_revision = view.world_revision;
	manifest.configuration_hash = view.configuration_hash;
	manifest.dependencies = view.dependencies;
	manifest.pages = view.pages;
	std::vector<std::uint8_t> migrated_world;
	if (wt_write_world_manifest(manifest, migrated_world) !=
		WtWorldManifestStatus::Ok) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	return publish(output_directory, migrated_world, pages, output);
}

WtWorldSnapshotStoreStatus wt_write_compacted_world_snapshot(
	WtAsyncStorageService &storage,
	const WtEditJournal &journal,
	const std::filesystem::path &output_directory,
	std::uint64_t new_source_revision,
	WtWorldSnapshotStoreResult &output
) {
	output = {};
	if (!journal.initialized() ||
		journal.source_revision() != storage.source_revision() ||
		journal.initial_world_revision() != storage.world_revision()) {
		return WtWorldSnapshotStoreStatus::ManifestFailure;
	}
	if (journal.transaction_count() == 0) {
		return WtWorldSnapshotStoreStatus::JournalEmpty;
	}
	if (storage.is_procedural()) {
		if (new_source_revision <= storage.source_revision()) {
			return WtWorldSnapshotStoreStatus::InvalidInput;
		}
		if (wt_should_inject_fault(
				WtFaultInjectionSite::SnapshotWorkspaceAllocation
			)) {
			return WtWorldSnapshotStoreStatus::AllocationFailure;
		}
		return write_procedural_snapshot(
			storage,
			journal,
			output_directory,
			new_source_revision,
			true,
			output
		);
	}
	if (wt_should_inject_fault(
			WtFaultInjectionSite::SnapshotWorkspaceAllocation
		)) {
		return WtWorldSnapshotStoreStatus::AllocationFailure;
	}
	std::vector<std::uint8_t> previous_world;
	WtWorldManifestView view;
	std::vector<WtBakedChunkPage> pages;
	const WtWorldSnapshotStoreStatus read = read_source(
		storage, previous_world, view, pages
	);
	if (read != WtWorldSnapshotStoreStatus::Ok) return read;
	WtCompactedSnapshot compacted;
	if (wt_compact_snapshot(
			{ previous_world.data(), previous_world.size() },
			pages,
			journal,
			new_source_revision,
			view.pages.size(),
			compacted
		) != WtSnapshotCompactionStatus::Ok) {
		return WtWorldSnapshotStoreStatus::CompactionFailure;
	}
	return publish(
		output_directory,
		compacted.world_bytes,
		compacted.pages,
		output
	);
}

} // namespace world_transvoxel
