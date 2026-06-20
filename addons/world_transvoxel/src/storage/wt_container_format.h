#pragma once

#include "storage/wt_binary_io.h"
#include "storage/wt_hash256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

constexpr std::uint16_t kWtFormatMajor = 1;
constexpr std::uint16_t kWtFormatMinor = 0;
constexpr std::size_t kWtContainerHeaderSize = 80;
constexpr std::size_t kWtSectionDirectoryEntrySize = 68;
constexpr std::size_t kWtMaximumContainerSize = 256U * 1024U * 1024U;
constexpr std::size_t kWtMaximumSectionSize = 64U * 1024U * 1024U;
constexpr std::size_t kWtMaximumSectionCount = 4096;
constexpr std::uint64_t kWtRequiredFeatureMask = 0xFFFFFFFF00000000ULL;
constexpr std::uint64_t kWtKnownRequiredFeatures = 0;

using WtFormatMagic = std::array<std::uint8_t, 8>;

inline constexpr WtFormatMagic kWtWorldMagic = {
	'W', 'T', 'W', 'O', 'R', 'L', 'D', 0
};
inline constexpr WtFormatMagic kWtChunkMagic = {
	'W', 'T', 'C', 'H', 'U', 'N', 'K', 0
};
inline constexpr WtFormatMagic kWtEditMagic = {
	'W', 'T', 'E', 'D', 'I', 'T', 0, 0
};
inline constexpr WtFormatMagic kWtTraceMagic = {
	'W', 'T', 'T', 'R', 'A', 'C', 'E', 0
};

enum class WtStorageCodec : std::uint16_t {
	None = 0,
};

enum class WtContainerStatus : std::uint8_t {
	Ok,
	InvalidInput,
	CapacityExceeded,
	Truncated,
	InvalidMagic,
	UnsupportedVersion,
	UnsupportedRequiredFeature,
	UnsupportedCodec,
	InvalidDirectory,
	DuplicateSection,
	InvalidSectionBounds,
	HashMismatch,
};

constexpr std::uint32_t wt_fourcc(char a, char b, char c, char d) noexcept {
	return
		static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
		(static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8) |
		(static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16) |
		(static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
}

struct WtContainerSectionInput {
	std::uint32_t type = 0;
	std::uint32_t flags = 0;
	WtStorageCodec codec = WtStorageCodec::None;
	WtByteView payload;
};

struct WtContainerHeader {
	WtFormatMagic magic{};
	std::uint16_t format_major = 0;
	std::uint16_t format_minor = 0;
	std::uint32_t header_size = 0;
	std::uint64_t feature_flags = 0;
	std::uint64_t source_revision = 0;
	std::uint64_t directory_offset = 0;
	std::uint64_t directory_size = 0;
	WtHash256 payload_hash{};
};

struct WtContainerSection {
	std::uint32_t type = 0;
	std::uint32_t flags = 0;
	WtStorageCodec codec = WtStorageCodec::None;
	std::uint64_t offset = 0;
	std::uint64_t stored_size = 0;
	std::uint64_t uncompressed_size = 0;
	WtHash256 content_hash{};
	WtByteView payload;
};

struct WtContainerView {
	WtContainerHeader header;
	std::vector<WtContainerSection> sections;

	const WtContainerSection *find_section(std::uint32_t type) const noexcept;
};

WtContainerStatus wt_write_container(
	const WtFormatMagic &magic,
	std::uint64_t feature_flags,
	std::uint64_t source_revision,
	const std::vector<WtContainerSectionInput> &sections,
	std::vector<std::uint8_t> &output
);

WtContainerStatus wt_read_container(
	WtByteView bytes,
	const WtFormatMagic &expected_magic,
	WtContainerView &output
);

WtContainerStatus wt_measure_container(
	WtByteView bytes,
	const WtFormatMagic &expected_magic,
	std::size_t &output_size
);

} // namespace world_transvoxel
