#pragma once

#include "core/wt_chunk_key.h"
#include "storage/wt_container_format.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace world_transvoxel {

constexpr std::uint16_t kWtWorldSchemaMajor = 1;
constexpr std::uint16_t kWtWorldSchemaMinor = 1;
constexpr std::size_t kWtWorldMetadataV1_0Size = 64;
constexpr std::size_t kWtWorldMetadataSize = 72;
constexpr std::size_t kWtWorldIndexHeaderSize = 12;
constexpr std::size_t kWtWorldIndexEntrySize = 56;
constexpr std::size_t kWtWorldDependencyHeaderSize = 8;
constexpr std::size_t kWtWorldDependencyEntryHeaderSize = 40;
constexpr std::size_t kWtMaximumWorldPageCount = 262144;
constexpr std::size_t kWtMaximumDependencyCount = 1024;
constexpr std::size_t kWtMaximumDependencyTextSize = 255;
constexpr std::uint32_t kWtWorldMetadataSection =
	wt_fourcc('M', 'E', 'T', 'A');
constexpr std::uint32_t kWtWorldDependencySection =
	wt_fourcc('D', 'E', 'P', 'S');
constexpr std::uint32_t kWtWorldIndexSection =
	wt_fourcc('I', 'N', 'D', 'X');

enum class WtDependencyKind : std::uint16_t {
	SourceAsset = 1,
	Generator = 2,
	Configuration = 3,
	Backend = 4,
	Godot = 5,
	GodotCpp = 6,
	Toolchain = 7,
};

struct WtDependencyEntry {
	WtDependencyKind kind = WtDependencyKind::SourceAsset;
	std::string label;
	std::string version;
	WtHash256 content_hash{};
};

struct WtWorldPageIndexEntry {
	WtChunkKey key;
	std::uint64_t byte_size = 0;
	WtHash256 content_hash{};

	bool operator==(const WtWorldPageIndexEntry &other) const noexcept;
};

struct WtWorldManifest {
	std::uint64_t source_revision = 0;
	std::uint64_t world_revision = 0;
	WtHash256 configuration_hash{};
	std::vector<WtDependencyEntry> dependencies;
	std::vector<WtWorldPageIndexEntry> pages;
};

struct WtWorldManifestView {
	WtContainerView container;
	std::uint64_t source_revision = 0;
	std::uint64_t world_revision = 0;
	WtHash256 configuration_hash{};
	std::vector<WtDependencyEntry> dependencies;
	std::vector<WtWorldPageIndexEntry> pages;

	const WtWorldPageIndexEntry *find_page(
		const WtChunkKey &key
	) const noexcept;
};

enum class WtWorldManifestStatus : std::uint8_t {
	Ok,
	InvalidInput,
	InvalidManifest,
	CapacityExceeded,
	ContainerFailure,
};

enum class WtWorldPageStatus : std::uint8_t {
	Ok,
	PageNotFound,
	SizeMismatch,
	HashMismatch,
	PageFailure,
	MetadataMismatch,
};

WtWorldManifestStatus wt_write_world_manifest(
	const WtWorldManifest &manifest,
	std::vector<std::uint8_t> &output
);

WtWorldManifestStatus wt_open_world_manifest(
	WtByteView bytes,
	WtWorldManifestView &output
);

WtWorldPageStatus wt_validate_world_page(
	const WtWorldManifestView &manifest,
	const WtChunkKey &key,
	WtByteView page_bytes
);

} // namespace world_transvoxel
