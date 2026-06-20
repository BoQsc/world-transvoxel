#include "storage/wt_world_manifest.h"

#include "core/wt_meshing_limits.h"
#include "storage/wt_binary_io.h"
#include "storage/wt_chunk_page.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <utility>

namespace world_transvoxel {
namespace {

bool valid_dependency_kind(WtDependencyKind kind) noexcept {
	const std::uint16_t value = static_cast<std::uint16_t>(kind);
	return value >= static_cast<std::uint16_t>(WtDependencyKind::SourceAsset) &&
		value <= static_cast<std::uint16_t>(WtDependencyKind::Toolchain);
}

bool valid_utf8(const std::string &text) noexcept {
	std::size_t index = 0;
	while (index < text.size()) {
		const std::uint8_t first = static_cast<std::uint8_t>(text[index]);
		std::size_t count = 0;
		std::uint32_t codepoint = 0;
		if (first <= 0x7fU) {
			count = 1;
			codepoint = first;
		} else if (first >= 0xc2U && first <= 0xdfU) {
			count = 2;
			codepoint = first & 0x1fU;
		} else if (first >= 0xe0U && first <= 0xefU) {
			count = 3;
			codepoint = first & 0x0fU;
		} else if (first >= 0xf0U && first <= 0xf4U) {
			count = 4;
			codepoint = first & 0x07U;
		} else {
			return false;
		}
		if (count > text.size() - index) {
			return false;
		}
		for (std::size_t offset = 1; offset < count; ++offset) {
			const std::uint8_t continuation =
				static_cast<std::uint8_t>(text[index + offset]);
			if ((continuation & 0xc0U) != 0x80U) {
				return false;
			}
			codepoint = (codepoint << 6) | (continuation & 0x3fU);
		}
		if ((count == 3 && codepoint < 0x800U) ||
			(count == 4 && codepoint < 0x10000U) ||
			codepoint < 0x20U || codepoint == 0x7fU ||
			(codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
			codepoint > 0x10ffffU) {
			return false;
		}
		index += count;
	}
	return true;
}

bool valid_dependency(const WtDependencyEntry &entry) noexcept {
	const bool version_required =
		entry.kind != WtDependencyKind::SourceAsset;
	return valid_dependency_kind(entry.kind) &&
		!entry.label.empty() &&
		entry.label.size() <= kWtMaximumDependencyTextSize &&
		entry.version.size() <= kWtMaximumDependencyTextSize &&
		(!version_required || !entry.version.empty()) &&
		valid_utf8(entry.label) &&
		valid_utf8(entry.version) &&
		!wt_is_zero_hash(entry.content_hash);
}

bool dependency_less(
	const WtDependencyEntry &left,
	const WtDependencyEntry &right
) noexcept {
	return std::tie(left.kind, left.label, left.version, left.content_hash) <
		std::tie(right.kind, right.label, right.version, right.content_hash);
}

bool dependency_identity_equal(
	const WtDependencyEntry &left,
	const WtDependencyEntry &right
) noexcept {
	return left.kind == right.kind && left.label == right.label;
}

bool valid_page(const WtWorldPageIndexEntry &entry) noexcept {
	return wt_is_valid_chunk_key(entry.key) &&
		entry.byte_size >= kWtContainerHeaderSize &&
		entry.byte_size <= kWtMaximumContainerSize &&
		!wt_is_zero_hash(entry.content_hash);
}

bool page_less(
	const WtWorldPageIndexEntry &left,
	const WtWorldPageIndexEntry &right
) noexcept {
	return left.key < right.key;
}

bool complete_dependencies(
	const std::vector<WtDependencyEntry> &dependencies,
	const WtHash256 &configuration_hash
) noexcept {
	std::array<std::size_t, 8> counts{};
	bool configuration_matches = false;
	for (const WtDependencyEntry &entry : dependencies) {
		const std::size_t index = static_cast<std::size_t>(entry.kind);
		++counts[index];
		if (entry.kind == WtDependencyKind::Configuration &&
			entry.content_hash == configuration_hash) {
			configuration_matches = true;
		}
	}
	for (std::size_t index =
			static_cast<std::size_t>(WtDependencyKind::Generator);
		index <= static_cast<std::size_t>(WtDependencyKind::Toolchain);
		++index) {
		if (counts[index] != 1) {
			return false;
		}
	}
	return configuration_matches;
}

WtWorldManifestStatus encode_metadata(
	const WtWorldManifest &manifest,
	std::vector<std::uint8_t> &output
) {
	WtBinaryWriter writer(kWtWorldMetadataSize);
	if (writer.write_u16(kWtWorldSchemaMajor) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtWorldSchemaMinor) != WtBinaryStatus::Ok ||
		writer.write_u32(static_cast<std::uint32_t>(manifest.pages.size())) !=
			WtBinaryStatus::Ok ||
		writer.write_u32(
			static_cast<std::uint32_t>(manifest.dependencies.size())
		) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtDefaultChunkCellsPerAxis) != WtBinaryStatus::Ok ||
		writer.write_u8(kWtMaximumLod) != WtBinaryStatus::Ok ||
		writer.write_u8(0) != WtBinaryStatus::Ok ||
		writer.write_u64(manifest.source_revision) != WtBinaryStatus::Ok ||
		writer.write_bytes(
			manifest.configuration_hash.data(),
			manifest.configuration_hash.size()
		) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtChunkPageSchemaMajor) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtChunkPageSchemaMinor) != WtBinaryStatus::Ok ||
		writer.write_u32(0) != WtBinaryStatus::Ok ||
		writer.write_u64(manifest.world_revision) != WtBinaryStatus::Ok) {
		return WtWorldManifestStatus::CapacityExceeded;
	}
	output = writer.take_bytes();
	return output.size() == kWtWorldMetadataSize ?
		WtWorldManifestStatus::Ok : WtWorldManifestStatus::CapacityExceeded;
}

WtWorldManifestStatus encode_dependencies(
	const std::vector<WtDependencyEntry> &dependencies,
	std::vector<std::uint8_t> &output
) {
	std::size_t size = kWtWorldDependencyHeaderSize;
	for (const WtDependencyEntry &entry : dependencies) {
		size += kWtWorldDependencyEntryHeaderSize +
			entry.label.size() + entry.version.size();
	}
	if (size > kWtMaximumSectionSize) {
		return WtWorldManifestStatus::CapacityExceeded;
	}
	WtBinaryWriter writer(size);
	if (writer.write_u16(kWtWorldSchemaMajor) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtWorldSchemaMinor) != WtBinaryStatus::Ok ||
		writer.write_u32(static_cast<std::uint32_t>(dependencies.size())) !=
			WtBinaryStatus::Ok) {
		return WtWorldManifestStatus::CapacityExceeded;
	}
	for (const WtDependencyEntry &entry : dependencies) {
		if (writer.write_u16(static_cast<std::uint16_t>(entry.kind)) !=
				WtBinaryStatus::Ok ||
			writer.write_u16(0) != WtBinaryStatus::Ok ||
			writer.write_u16(static_cast<std::uint16_t>(entry.label.size())) !=
				WtBinaryStatus::Ok ||
			writer.write_u16(static_cast<std::uint16_t>(entry.version.size())) !=
				WtBinaryStatus::Ok ||
			writer.write_bytes(entry.content_hash.data(), entry.content_hash.size()) !=
				WtBinaryStatus::Ok ||
			writer.write_bytes(
				reinterpret_cast<const std::uint8_t *>(entry.label.data()),
				entry.label.size()
			) != WtBinaryStatus::Ok ||
			writer.write_bytes(
				reinterpret_cast<const std::uint8_t *>(entry.version.data()),
				entry.version.size()
			) != WtBinaryStatus::Ok) {
			return WtWorldManifestStatus::CapacityExceeded;
		}
	}
	output = writer.take_bytes();
	return WtWorldManifestStatus::Ok;
}

WtWorldManifestStatus encode_index(
	const std::vector<WtWorldPageIndexEntry> &pages,
	std::vector<std::uint8_t> &output
) {
	const std::size_t size =
		kWtWorldIndexHeaderSize + pages.size() * kWtWorldIndexEntrySize;
	WtBinaryWriter writer(size);
	if (writer.write_u16(kWtWorldSchemaMajor) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtWorldSchemaMinor) != WtBinaryStatus::Ok ||
		writer.write_u32(static_cast<std::uint32_t>(pages.size())) !=
			WtBinaryStatus::Ok ||
		writer.write_u16(kWtWorldIndexEntrySize) != WtBinaryStatus::Ok ||
		writer.write_u16(0) != WtBinaryStatus::Ok) {
		return WtWorldManifestStatus::CapacityExceeded;
	}
	for (const WtWorldPageIndexEntry &page : pages) {
		if (writer.write_i32(page.key.x) != WtBinaryStatus::Ok ||
			writer.write_i32(page.key.y) != WtBinaryStatus::Ok ||
			writer.write_i32(page.key.z) != WtBinaryStatus::Ok ||
			writer.write_u8(page.key.lod) != WtBinaryStatus::Ok ||
			writer.write_u8(0) != WtBinaryStatus::Ok ||
			writer.write_u16(0) != WtBinaryStatus::Ok ||
			writer.write_u64(page.byte_size) != WtBinaryStatus::Ok ||
			writer.write_bytes(page.content_hash.data(), page.content_hash.size()) !=
				WtBinaryStatus::Ok) {
			return WtWorldManifestStatus::CapacityExceeded;
		}
	}
	output = writer.take_bytes();
	return WtWorldManifestStatus::Ok;
}

bool read_schema_header(
	WtBinaryReader &reader,
	std::uint32_t &count
) {
	std::uint16_t major = 0;
	std::uint16_t minor = 0;
	return reader.read_u16(major) == WtBinaryStatus::Ok &&
		reader.read_u16(minor) == WtBinaryStatus::Ok &&
		reader.read_u32(count) == WtBinaryStatus::Ok &&
		major == kWtWorldSchemaMajor &&
		minor <= kWtWorldSchemaMinor;
}

bool decode_metadata(
	WtByteView bytes,
	WtWorldManifestView &output,
	std::uint32_t &page_count,
	std::uint32_t &dependency_count
) {
	if (bytes.size != kWtWorldMetadataV1_0Size &&
		bytes.size != kWtWorldMetadataSize) {
		return false;
	}
	WtBinaryReader reader(bytes);
	std::uint16_t major = 0;
	std::uint16_t minor = 0;
	std::uint16_t chunk_cells = 0;
	std::uint8_t maximum_lod = 0;
	std::uint8_t reserved_byte = 0;
	std::uint16_t page_major = 0;
	std::uint16_t page_minor = 0;
	std::uint32_t reserved = 0;
	WtByteView hash;
	if (reader.read_u16(major) != WtBinaryStatus::Ok ||
		reader.read_u16(minor) != WtBinaryStatus::Ok ||
		reader.read_u32(page_count) != WtBinaryStatus::Ok ||
		reader.read_u32(dependency_count) != WtBinaryStatus::Ok ||
		reader.read_u16(chunk_cells) != WtBinaryStatus::Ok ||
		reader.read_u8(maximum_lod) != WtBinaryStatus::Ok ||
		reader.read_u8(reserved_byte) != WtBinaryStatus::Ok ||
		reader.read_u64(output.source_revision) != WtBinaryStatus::Ok ||
		reader.read_bytes(output.configuration_hash.size(), hash) !=
			WtBinaryStatus::Ok ||
		reader.read_u16(page_major) != WtBinaryStatus::Ok ||
		reader.read_u16(page_minor) != WtBinaryStatus::Ok ||
		reader.read_u32(reserved) != WtBinaryStatus::Ok) {
		return false;
	}
	std::copy(
		hash.data,
		hash.data + hash.size,
		output.configuration_hash.begin()
	);
	if (major != kWtWorldSchemaMajor ||
		minor > kWtWorldSchemaMinor ||
		(minor == 0 && bytes.size != kWtWorldMetadataV1_0Size) ||
		(minor >= 1 && bytes.size != kWtWorldMetadataSize)) {
		return false;
	}
	if (minor >= 1) {
		if (reader.read_u64(output.world_revision) != WtBinaryStatus::Ok) {
			return false;
		}
	} else {
		output.world_revision = 0;
	}
	return
		minor <= kWtWorldSchemaMinor &&
		page_count <= kWtMaximumWorldPageCount &&
		dependency_count <= kWtMaximumDependencyCount &&
		chunk_cells == kWtDefaultChunkCellsPerAxis &&
		maximum_lod == kWtMaximumLod &&
		reserved_byte == 0 &&
		page_major == kWtChunkPageSchemaMajor &&
		page_minor <= kWtChunkPageSchemaMinor &&
		reserved == 0 &&
		reader.remaining() == 0 &&
		!wt_is_zero_hash(output.configuration_hash);
}

bool decode_dependencies(
	WtByteView bytes,
	std::uint32_t expected_count,
	std::vector<WtDependencyEntry> &output
) {
	WtBinaryReader reader(bytes);
	std::uint32_t count = 0;
	if (!read_schema_header(reader, count) ||
		count != expected_count ||
		count > kWtMaximumDependencyCount) {
		return false;
	}
	output.reserve(count);
	for (std::uint32_t index = 0; index < count; ++index) {
		std::uint16_t kind = 0;
		std::uint16_t flags = 0;
		std::uint16_t label_size = 0;
		std::uint16_t version_size = 0;
		WtByteView hash;
		WtByteView label;
		WtByteView version;
		if (reader.read_u16(kind) != WtBinaryStatus::Ok ||
			reader.read_u16(flags) != WtBinaryStatus::Ok ||
			reader.read_u16(label_size) != WtBinaryStatus::Ok ||
			reader.read_u16(version_size) != WtBinaryStatus::Ok ||
			reader.read_bytes(32, hash) != WtBinaryStatus::Ok ||
			reader.read_bytes(label_size, label) != WtBinaryStatus::Ok ||
			reader.read_bytes(version_size, version) != WtBinaryStatus::Ok ||
			flags != 0 ||
			label_size > kWtMaximumDependencyTextSize ||
			version_size > kWtMaximumDependencyTextSize) {
			return false;
		}
		WtDependencyEntry entry;
		entry.kind = static_cast<WtDependencyKind>(kind);
		entry.label.assign(
			reinterpret_cast<const char *>(label.data),
			label.size
		);
		entry.version.assign(
			reinterpret_cast<const char *>(version.data),
			version.size
		);
		std::copy(hash.data, hash.data + hash.size, entry.content_hash.begin());
		if (!valid_dependency(entry) ||
			(!output.empty() && !dependency_less(output.back(), entry))) {
			return false;
		}
		output.push_back(std::move(entry));
	}
	return reader.remaining() == 0;
}

bool decode_index(
	WtByteView bytes,
	std::uint32_t expected_count,
	std::vector<WtWorldPageIndexEntry> &output
) {
	WtBinaryReader reader(bytes);
	std::uint32_t count = 0;
	std::uint16_t entry_size = 0;
	std::uint16_t reserved_header = 0;
	if (!read_schema_header(reader, count) ||
		reader.read_u16(entry_size) != WtBinaryStatus::Ok ||
		reader.read_u16(reserved_header) != WtBinaryStatus::Ok ||
		count != expected_count ||
		count > kWtMaximumWorldPageCount ||
		entry_size != kWtWorldIndexEntrySize ||
		reserved_header != 0 ||
		bytes.size != kWtWorldIndexHeaderSize +
			static_cast<std::size_t>(count) * kWtWorldIndexEntrySize) {
		return false;
	}
	output.reserve(count);
	for (std::uint32_t index = 0; index < count; ++index) {
		WtWorldPageIndexEntry entry;
		std::uint8_t flags = 0;
		std::uint16_t reserved = 0;
		WtByteView hash;
		if (reader.read_i32(entry.key.x) != WtBinaryStatus::Ok ||
			reader.read_i32(entry.key.y) != WtBinaryStatus::Ok ||
			reader.read_i32(entry.key.z) != WtBinaryStatus::Ok ||
			reader.read_u8(entry.key.lod) != WtBinaryStatus::Ok ||
			reader.read_u8(flags) != WtBinaryStatus::Ok ||
			reader.read_u16(reserved) != WtBinaryStatus::Ok ||
			reader.read_u64(entry.byte_size) != WtBinaryStatus::Ok ||
			reader.read_bytes(32, hash) != WtBinaryStatus::Ok) {
			return false;
		}
		std::copy(hash.data, hash.data + hash.size, entry.content_hash.begin());
		if (flags != 0 || reserved != 0 || !valid_page(entry) ||
			(!output.empty() && !page_less(output.back(), entry))) {
			return false;
		}
		output.push_back(entry);
	}
	return reader.remaining() == 0;
}

} // namespace

bool WtWorldPageIndexEntry::operator==(
	const WtWorldPageIndexEntry &other
) const noexcept {
	return key == other.key &&
		byte_size == other.byte_size &&
		content_hash == other.content_hash;
}

const WtWorldPageIndexEntry *WtWorldManifestView::find_page(
	const WtChunkKey &key
) const noexcept {
	const auto iterator = std::lower_bound(
		pages.begin(),
		pages.end(),
		key,
		[](const WtWorldPageIndexEntry &entry, const WtChunkKey &value) {
			return entry.key < value;
		}
	);
	return iterator != pages.end() && iterator->key == key ?
		&*iterator : nullptr;
}

WtWorldManifestStatus wt_write_world_manifest(
	const WtWorldManifest &manifest,
	std::vector<std::uint8_t> &output
) {
	output.clear();
	if (manifest.pages.size() > kWtMaximumWorldPageCount ||
		manifest.dependencies.size() > kWtMaximumDependencyCount ||
		wt_is_zero_hash(manifest.configuration_hash)) {
		return WtWorldManifestStatus::InvalidInput;
	}
	WtWorldManifest ordered = manifest;
	std::sort(
		ordered.dependencies.begin(),
		ordered.dependencies.end(),
		dependency_less
	);
	std::sort(ordered.pages.begin(), ordered.pages.end(), page_less);
	for (std::size_t index = 0; index < ordered.dependencies.size(); ++index) {
		if (!valid_dependency(ordered.dependencies[index]) ||
			(index != 0 && dependency_identity_equal(
				ordered.dependencies[index - 1],
				ordered.dependencies[index]
			))) {
			return WtWorldManifestStatus::InvalidInput;
		}
	}
	if (!complete_dependencies(
			ordered.dependencies,
			ordered.configuration_hash
		)) {
		return WtWorldManifestStatus::InvalidInput;
	}
	for (std::size_t index = 0; index < ordered.pages.size(); ++index) {
		if (!valid_page(ordered.pages[index]) ||
			(index != 0 &&
				ordered.pages[index - 1].key == ordered.pages[index].key)) {
			return WtWorldManifestStatus::InvalidInput;
		}
	}

	std::vector<std::uint8_t> metadata;
	std::vector<std::uint8_t> dependencies;
	std::vector<std::uint8_t> index;
	WtWorldManifestStatus status = encode_metadata(ordered, metadata);
	if (status != WtWorldManifestStatus::Ok) return status;
	status = encode_dependencies(ordered.dependencies, dependencies);
	if (status != WtWorldManifestStatus::Ok) return status;
	status = encode_index(ordered.pages, index);
	if (status != WtWorldManifestStatus::Ok) return status;

	const std::vector<WtContainerSectionInput> sections = {
		{ kWtWorldMetadataSection, 0, WtStorageCodec::None,
			{ metadata.data(), metadata.size() } },
		{ kWtWorldDependencySection, 0, WtStorageCodec::None,
			{ dependencies.data(), dependencies.size() } },
		{ kWtWorldIndexSection, 0, WtStorageCodec::None,
			{ index.data(), index.size() } },
	};
	return wt_write_container(
		kWtWorldMagic,
		0,
		ordered.source_revision,
		sections,
		output
	) == WtContainerStatus::Ok ?
		WtWorldManifestStatus::Ok : WtWorldManifestStatus::ContainerFailure;
}

WtWorldManifestStatus wt_open_world_manifest(
	WtByteView bytes,
	WtWorldManifestView &output
) {
	output = {};
	if (wt_read_container(bytes, kWtWorldMagic, output.container) !=
		WtContainerStatus::Ok) {
		return WtWorldManifestStatus::ContainerFailure;
	}
	const WtContainerSection *metadata =
		output.container.find_section(kWtWorldMetadataSection);
	const WtContainerSection *dependencies =
		output.container.find_section(kWtWorldDependencySection);
	const WtContainerSection *index =
		output.container.find_section(kWtWorldIndexSection);
	if (output.container.sections.size() != 3 ||
		metadata == nullptr || dependencies == nullptr || index == nullptr ||
		metadata->flags != 0 || dependencies->flags != 0 || index->flags != 0) {
		output = {};
		return WtWorldManifestStatus::InvalidManifest;
	}
	std::uint32_t page_count = 0;
	std::uint32_t dependency_count = 0;
	if (!decode_metadata(
			metadata->payload,
			output,
			page_count,
			dependency_count
		) ||
		output.source_revision != output.container.header.source_revision ||
		!decode_dependencies(
			dependencies->payload,
			dependency_count,
			output.dependencies
		) ||
		!complete_dependencies(
			output.dependencies,
			output.configuration_hash
		) ||
		!decode_index(index->payload, page_count, output.pages)) {
		output = {};
		return WtWorldManifestStatus::InvalidManifest;
	}
	return WtWorldManifestStatus::Ok;
}

WtWorldPageStatus wt_validate_world_page(
	const WtWorldManifestView &manifest,
	const WtChunkKey &key,
	WtByteView page_bytes
) {
	const WtWorldPageIndexEntry *entry = manifest.find_page(key);
	if (entry == nullptr) {
		return WtWorldPageStatus::PageNotFound;
	}
	if (page_bytes.size != entry->byte_size) {
		return WtWorldPageStatus::SizeMismatch;
	}
	if (page_bytes.size != 0 && page_bytes.data == nullptr) {
		return WtWorldPageStatus::PageFailure;
	}
	if (wt_sha256(page_bytes.data, page_bytes.size) != entry->content_hash) {
		return WtWorldPageStatus::HashMismatch;
	}
	WtChunkPageView page;
	if (wt_open_chunk_page(page_bytes, page) != WtChunkPageStatus::Ok) {
		return WtWorldPageStatus::PageFailure;
	}
	return page.metadata.key == key &&
		page.metadata.source_revision == manifest.source_revision ?
		WtWorldPageStatus::Ok : WtWorldPageStatus::MetadataMismatch;
}

} // namespace world_transvoxel
