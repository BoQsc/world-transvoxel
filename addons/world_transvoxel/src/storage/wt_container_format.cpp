#include "storage/wt_container_format.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace world_transvoxel {
namespace {

bool add_fits(std::uint64_t a, std::uint64_t b, std::uint64_t maximum) noexcept {
	return a <= maximum && b <= maximum - a;
}

bool valid_magic(const WtFormatMagic &magic) noexcept {
	return magic == kWtWorldMagic || magic == kWtChunkMagic ||
		magic == kWtEditMagic || magic == kWtTraceMagic;
}

WtContainerStatus write_header(
	WtBinaryWriter &writer,
	const WtContainerHeader &header
) {
	return
		writer.write_bytes(header.magic.data(), header.magic.size()) == WtBinaryStatus::Ok &&
		writer.write_u16(header.format_major) == WtBinaryStatus::Ok &&
		writer.write_u16(header.format_minor) == WtBinaryStatus::Ok &&
		writer.write_u32(header.header_size) == WtBinaryStatus::Ok &&
		writer.write_u64(header.feature_flags) == WtBinaryStatus::Ok &&
		writer.write_u64(header.source_revision) == WtBinaryStatus::Ok &&
		writer.write_u64(header.directory_offset) == WtBinaryStatus::Ok &&
		writer.write_u64(header.directory_size) == WtBinaryStatus::Ok &&
		writer.write_bytes(header.payload_hash.data(), header.payload_hash.size()) ==
			WtBinaryStatus::Ok ?
		WtContainerStatus::Ok : WtContainerStatus::CapacityExceeded;
}

WtContainerStatus read_header(
	WtBinaryReader &reader,
	WtContainerHeader &header
) {
	WtByteView magic;
	WtByteView hash;
	if (reader.read_bytes(header.magic.size(), magic) != WtBinaryStatus::Ok ||
		reader.read_u16(header.format_major) != WtBinaryStatus::Ok ||
		reader.read_u16(header.format_minor) != WtBinaryStatus::Ok ||
		reader.read_u32(header.header_size) != WtBinaryStatus::Ok ||
		reader.read_u64(header.feature_flags) != WtBinaryStatus::Ok ||
		reader.read_u64(header.source_revision) != WtBinaryStatus::Ok ||
		reader.read_u64(header.directory_offset) != WtBinaryStatus::Ok ||
		reader.read_u64(header.directory_size) != WtBinaryStatus::Ok ||
		reader.read_bytes(header.payload_hash.size(), hash) != WtBinaryStatus::Ok) {
		return WtContainerStatus::Truncated;
	}
	std::copy(magic.data, magic.data + magic.size, header.magic.begin());
	std::copy(hash.data, hash.data + hash.size, header.payload_hash.begin());
	return WtContainerStatus::Ok;
}

WtContainerStatus write_section_entry(
	WtBinaryWriter &writer,
	const WtContainerSection &section
) {
	return
		writer.write_u32(section.type) == WtBinaryStatus::Ok &&
		writer.write_u32(section.flags) == WtBinaryStatus::Ok &&
		writer.write_u16(static_cast<std::uint16_t>(section.codec)) == WtBinaryStatus::Ok &&
		writer.write_u16(0) == WtBinaryStatus::Ok &&
		writer.write_u64(section.offset) == WtBinaryStatus::Ok &&
		writer.write_u64(section.stored_size) == WtBinaryStatus::Ok &&
		writer.write_u64(section.uncompressed_size) == WtBinaryStatus::Ok &&
		writer.write_bytes(section.content_hash.data(), section.content_hash.size()) ==
			WtBinaryStatus::Ok ?
		WtContainerStatus::Ok : WtContainerStatus::CapacityExceeded;
}

WtContainerStatus read_section_entry(
	WtBinaryReader &reader,
	WtContainerSection &section
) {
	std::uint16_t codec = 0;
	std::uint16_t reserved = 0;
	WtByteView hash;
	if (reader.read_u32(section.type) != WtBinaryStatus::Ok ||
		reader.read_u32(section.flags) != WtBinaryStatus::Ok ||
		reader.read_u16(codec) != WtBinaryStatus::Ok ||
		reader.read_u16(reserved) != WtBinaryStatus::Ok ||
		reader.read_u64(section.offset) != WtBinaryStatus::Ok ||
		reader.read_u64(section.stored_size) != WtBinaryStatus::Ok ||
		reader.read_u64(section.uncompressed_size) != WtBinaryStatus::Ok ||
		reader.read_bytes(section.content_hash.size(), hash) != WtBinaryStatus::Ok) {
		return WtContainerStatus::Truncated;
	}
	if (reserved != 0) {
		return WtContainerStatus::InvalidDirectory;
	}
	section.codec = static_cast<WtStorageCodec>(codec);
	std::copy(hash.data, hash.data + hash.size, section.content_hash.begin());
	return WtContainerStatus::Ok;
}

} // namespace

const WtContainerSection *WtContainerView::find_section(
	std::uint32_t type
) const noexcept {
	const auto iterator = std::lower_bound(
		sections.begin(), sections.end(), type,
		[](const WtContainerSection &section, std::uint32_t value) {
			return section.type < value;
		}
	);
	return iterator != sections.end() && iterator->type == type ? &*iterator : nullptr;
}

WtContainerStatus wt_write_container(
	const WtFormatMagic &magic,
	std::uint64_t feature_flags,
	std::uint64_t source_revision,
	const std::vector<WtContainerSectionInput> &sections,
	std::vector<std::uint8_t> &output
) {
	output.clear();
	if (!valid_magic(magic) ||
		(feature_flags & kWtRequiredFeatureMask & ~kWtKnownRequiredFeatures) != 0 ||
		sections.size() > kWtMaximumSectionCount) {
		return WtContainerStatus::InvalidInput;
	}

	std::vector<WtContainerSectionInput> ordered = sections;
	std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
		return a.type < b.type;
	});
	for (std::size_t index = 0; index < ordered.size(); ++index) {
		if (ordered[index].type == 0 ||
			ordered[index].codec != WtStorageCodec::None ||
			ordered[index].payload.size > kWtMaximumSectionSize ||
			(ordered[index].payload.size != 0 && ordered[index].payload.data == nullptr)) {
			return ordered[index].codec != WtStorageCodec::None ?
				WtContainerStatus::UnsupportedCodec : WtContainerStatus::InvalidInput;
		}
		if (index != 0 && ordered[index - 1].type == ordered[index].type) {
			return WtContainerStatus::DuplicateSection;
		}
	}

	const std::uint64_t directory_size =
		static_cast<std::uint64_t>(ordered.size()) * kWtSectionDirectoryEntrySize;
	std::uint64_t total_size = kWtContainerHeaderSize;
	if (!add_fits(total_size, directory_size, kWtMaximumContainerSize)) {
		return WtContainerStatus::CapacityExceeded;
	}
	total_size += directory_size;

	std::vector<WtContainerSection> directory;
	directory.reserve(ordered.size());
	for (const WtContainerSectionInput &input : ordered) {
		if (!add_fits(total_size, input.payload.size, kWtMaximumContainerSize)) {
			return WtContainerStatus::CapacityExceeded;
		}
		directory.push_back({
			input.type,
			input.flags,
			input.codec,
			total_size,
			input.payload.size,
			input.payload.size,
			wt_sha256(input.payload.data, input.payload.size),
			{},
		});
		total_size += input.payload.size;
	}

	WtBinaryWriter writer(static_cast<std::size_t>(total_size));
	WtContainerHeader header = {
		magic,
		kWtFormatMajor,
		kWtFormatMinor,
		static_cast<std::uint32_t>(kWtContainerHeaderSize),
		feature_flags,
		source_revision,
		kWtContainerHeaderSize,
		directory_size,
		{},
	};
	if (write_header(writer, header) != WtContainerStatus::Ok) {
		return WtContainerStatus::CapacityExceeded;
	}
	for (const WtContainerSection &section : directory) {
		if (write_section_entry(writer, section) != WtContainerStatus::Ok) {
			return WtContainerStatus::CapacityExceeded;
		}
	}
	for (const WtContainerSectionInput &section : ordered) {
		if (writer.write_bytes(section.payload.data, section.payload.size) != WtBinaryStatus::Ok) {
			return WtContainerStatus::CapacityExceeded;
		}
	}
	const std::vector<std::uint8_t> &bytes = writer.bytes();
	header.payload_hash = wt_sha256(
		bytes.data() + header.directory_offset,
		bytes.size() - static_cast<std::size_t>(header.directory_offset)
	);
	if (writer.patch_bytes(48, header.payload_hash.data(), header.payload_hash.size()) !=
		WtBinaryStatus::Ok) {
		return WtContainerStatus::CapacityExceeded;
	}
	output = writer.take_bytes();
	return WtContainerStatus::Ok;
}

WtContainerStatus wt_read_container(
	WtByteView bytes,
	const WtFormatMagic &expected_magic,
	WtContainerView &output
) {
	output = {};
	if (bytes.size > kWtMaximumContainerSize || (bytes.size != 0 && bytes.data == nullptr)) {
		return WtContainerStatus::InvalidInput;
	}
	if (bytes.size < kWtContainerHeaderSize) {
		return WtContainerStatus::Truncated;
	}
	WtBinaryReader reader(bytes);
	WtContainerHeader header;
	WtContainerStatus status = read_header(reader, header);
	if (status != WtContainerStatus::Ok) {
		return status;
	}
	if (header.magic != expected_magic) {
		return WtContainerStatus::InvalidMagic;
	}
	if (header.format_major != kWtFormatMajor || header.format_minor > kWtFormatMinor) {
		return WtContainerStatus::UnsupportedVersion;
	}
	if (header.header_size != kWtContainerHeaderSize ||
		header.directory_offset != kWtContainerHeaderSize ||
		(header.directory_size % kWtSectionDirectoryEntrySize) != 0) {
		return WtContainerStatus::InvalidDirectory;
	}
	if ((header.feature_flags & kWtRequiredFeatureMask & ~kWtKnownRequiredFeatures) != 0) {
		return WtContainerStatus::UnsupportedRequiredFeature;
	}
	if (!add_fits(header.directory_offset, header.directory_size, bytes.size)) {
		return WtContainerStatus::InvalidDirectory;
	}
	const std::uint64_t section_count =
		header.directory_size / kWtSectionDirectoryEntrySize;
	if (section_count > kWtMaximumSectionCount) {
		return WtContainerStatus::InvalidDirectory;
	}
	if (wt_sha256(
			bytes.data + header.directory_offset,
			bytes.size - static_cast<std::size_t>(header.directory_offset)
		) != header.payload_hash) {
		return WtContainerStatus::HashMismatch;
	}

	output.header = header;
	output.sections.reserve(static_cast<std::size_t>(section_count));
	if (reader.seek(static_cast<std::size_t>(header.directory_offset)) != WtBinaryStatus::Ok) {
		return WtContainerStatus::InvalidDirectory;
	}
	std::uint64_t previous_end = header.directory_offset + header.directory_size;
	for (std::uint64_t index = 0; index < section_count; ++index) {
		WtContainerSection section;
		status = read_section_entry(reader, section);
		if (status != WtContainerStatus::Ok) {
			output = {};
			return status;
		}
		if (section.codec != WtStorageCodec::None) {
			output = {};
			return WtContainerStatus::UnsupportedCodec;
		}
		if (section.type == 0 ||
			section.stored_size != section.uncompressed_size ||
			section.stored_size > kWtMaximumSectionSize ||
			section.offset < previous_end ||
			!add_fits(section.offset, section.stored_size, bytes.size)) {
			output = {};
			return WtContainerStatus::InvalidSectionBounds;
		}
		if (!output.sections.empty() &&
			output.sections.back().type >= section.type) {
			const bool duplicate = output.sections.back().type == section.type;
			output = {};
			return duplicate ?
				WtContainerStatus::DuplicateSection :
				WtContainerStatus::InvalidDirectory;
		}
		section.payload = {
			bytes.data + section.offset,
			static_cast<std::size_t>(section.stored_size),
		};
		if (wt_sha256(section.payload.data, section.payload.size) != section.content_hash) {
			output = {};
			return WtContainerStatus::HashMismatch;
		}
		previous_end = section.offset + section.stored_size;
		output.sections.push_back(section);
	}
	if (previous_end != bytes.size) {
		output = {};
		return WtContainerStatus::InvalidSectionBounds;
	}
	return WtContainerStatus::Ok;
}

WtContainerStatus wt_measure_container(
	WtByteView bytes,
	const WtFormatMagic &expected_magic,
	std::size_t &output_size
) {
	output_size = 0;
	if (bytes.size != 0 && bytes.data == nullptr) {
		return WtContainerStatus::InvalidInput;
	}
	if (bytes.size < kWtContainerHeaderSize) {
		return WtContainerStatus::Truncated;
	}
	WtBinaryReader reader(bytes);
	WtContainerHeader header;
	WtContainerStatus status = read_header(reader, header);
	if (status != WtContainerStatus::Ok) return status;
	if (header.magic != expected_magic) {
		return WtContainerStatus::InvalidMagic;
	}
	if (header.format_major != kWtFormatMajor ||
		header.format_minor > kWtFormatMinor) {
		return WtContainerStatus::UnsupportedVersion;
	}
	if (header.header_size != kWtContainerHeaderSize ||
		header.directory_offset != kWtContainerHeaderSize ||
		(header.directory_size % kWtSectionDirectoryEntrySize) != 0) {
		return WtContainerStatus::InvalidDirectory;
	}
	if ((header.feature_flags & kWtRequiredFeatureMask &
			~kWtKnownRequiredFeatures) != 0) {
		return WtContainerStatus::UnsupportedRequiredFeature;
	}
	if (!add_fits(
			header.directory_offset,
			header.directory_size,
			kWtMaximumContainerSize
		)) {
		return WtContainerStatus::InvalidDirectory;
	}
	const std::uint64_t directory_end =
		header.directory_offset + header.directory_size;
	if (directory_end > bytes.size) {
		return WtContainerStatus::Truncated;
	}
	const std::uint64_t section_count =
		header.directory_size / kWtSectionDirectoryEntrySize;
	if (section_count > kWtMaximumSectionCount ||
		reader.seek(static_cast<std::size_t>(header.directory_offset)) !=
			WtBinaryStatus::Ok) {
		return WtContainerStatus::InvalidDirectory;
	}
	std::uint64_t previous_end = directory_end;
	std::uint32_t previous_type = 0;
	for (std::uint64_t index = 0; index < section_count; ++index) {
		WtContainerSection section;
		status = read_section_entry(reader, section);
		if (status != WtContainerStatus::Ok) return status;
		if (section.codec != WtStorageCodec::None) {
			return WtContainerStatus::UnsupportedCodec;
		}
		if (section.type == 0 ||
			section.stored_size != section.uncompressed_size ||
			section.stored_size > kWtMaximumSectionSize ||
			section.offset != previous_end ||
			!add_fits(
				section.offset,
				section.stored_size,
				kWtMaximumContainerSize
			)) {
			return WtContainerStatus::InvalidSectionBounds;
		}
		if (index != 0 && previous_type >= section.type) {
			return previous_type == section.type ?
				WtContainerStatus::DuplicateSection :
				WtContainerStatus::InvalidDirectory;
		}
		previous_type = section.type;
		previous_end = section.offset + section.stored_size;
	}
	if (previous_end > bytes.size) {
		return WtContainerStatus::Truncated;
	}
	output_size = static_cast<std::size_t>(previous_end);
	return WtContainerStatus::Ok;
}

} // namespace world_transvoxel
