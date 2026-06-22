#include "storage/wt_chunk_page.h"

#include "storage/wt_binary_io.h"

#include <cmath>

namespace world_transvoxel {
namespace {

bool valid_metadata(const WtChunkPageMetadata &metadata) noexcept {
	return wt_is_valid_chunk_key(metadata.key) &&
		metadata.sample_minimum == -1 &&
		metadata.sample_maximum == 17 &&
		metadata.dimension_x == kWtChunkMeshingSamplesPerAxis &&
		metadata.dimension_y == kWtChunkMeshingSamplesPerAxis &&
		metadata.dimension_z == kWtChunkMeshingSamplesPerAxis &&
		metadata.density_encoding == WtDensityEncoding::Float32 &&
		metadata.material_encoding == WtMaterialEncoding::Uint16 &&
		metadata.sample_count == kWtChunkPageSampleCount &&
		metadata.cell_spacing == static_cast<std::uint64_t>(
			wt_lod_cell_size(metadata.key.lod)
		);
}

WtChunkPageStatus encode_header(
	const WtChunkPageMetadata &metadata,
	std::vector<std::uint8_t> &output
) {
	WtBinaryWriter writer(kWtChunkPageHeaderSize);
	if (writer.write_u16(kWtChunkPageSchemaMajor) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtChunkPageSchemaMinor) != WtBinaryStatus::Ok ||
		writer.write_i32(metadata.key.x) != WtBinaryStatus::Ok ||
		writer.write_i32(metadata.key.y) != WtBinaryStatus::Ok ||
		writer.write_i32(metadata.key.z) != WtBinaryStatus::Ok ||
		writer.write_u8(metadata.key.lod) != WtBinaryStatus::Ok ||
		writer.write_u8(static_cast<std::uint8_t>(metadata.sample_minimum)) !=
			WtBinaryStatus::Ok ||
		writer.write_u8(static_cast<std::uint8_t>(metadata.sample_maximum)) !=
			WtBinaryStatus::Ok ||
		writer.write_u8(0) != WtBinaryStatus::Ok ||
		writer.write_u16(metadata.dimension_x) != WtBinaryStatus::Ok ||
		writer.write_u16(metadata.dimension_y) != WtBinaryStatus::Ok ||
		writer.write_u16(metadata.dimension_z) != WtBinaryStatus::Ok ||
		writer.write_u8(static_cast<std::uint8_t>(metadata.density_encoding)) !=
			WtBinaryStatus::Ok ||
		writer.write_u8(static_cast<std::uint8_t>(metadata.material_encoding)) !=
			WtBinaryStatus::Ok ||
		writer.write_u32(metadata.sample_count) != WtBinaryStatus::Ok ||
		writer.write_u64(metadata.cell_spacing) != WtBinaryStatus::Ok ||
		writer.write_u64(metadata.source_revision) != WtBinaryStatus::Ok) {
		return WtChunkPageStatus::CapacityExceeded;
	}
	output = writer.take_bytes();
	return output.size() == kWtChunkPageHeaderSize ?
		WtChunkPageStatus::Ok : WtChunkPageStatus::CapacityExceeded;
}

WtChunkPageStatus decode_header(
	WtByteView bytes,
	WtChunkPageMetadata &metadata
) {
	if (bytes.size != kWtChunkPageHeaderSize) {
		return WtChunkPageStatus::InvalidMetadata;
	}
	WtBinaryReader reader(bytes);
	std::uint16_t major = 0;
	std::uint16_t minor = 0;
	std::uint8_t lod = 0;
	std::uint8_t sample_minimum = 0;
	std::uint8_t sample_maximum = 0;
	std::uint8_t reserved = 0;
	std::uint8_t density_encoding = 0;
	std::uint8_t material_encoding = 0;
	if (reader.read_u16(major) != WtBinaryStatus::Ok ||
		reader.read_u16(minor) != WtBinaryStatus::Ok ||
		reader.read_i32(metadata.key.x) != WtBinaryStatus::Ok ||
		reader.read_i32(metadata.key.y) != WtBinaryStatus::Ok ||
		reader.read_i32(metadata.key.z) != WtBinaryStatus::Ok ||
		reader.read_u8(lod) != WtBinaryStatus::Ok ||
		reader.read_u8(sample_minimum) != WtBinaryStatus::Ok ||
		reader.read_u8(sample_maximum) != WtBinaryStatus::Ok ||
		reader.read_u8(reserved) != WtBinaryStatus::Ok ||
		reader.read_u16(metadata.dimension_x) != WtBinaryStatus::Ok ||
		reader.read_u16(metadata.dimension_y) != WtBinaryStatus::Ok ||
		reader.read_u16(metadata.dimension_z) != WtBinaryStatus::Ok ||
		reader.read_u8(density_encoding) != WtBinaryStatus::Ok ||
		reader.read_u8(material_encoding) != WtBinaryStatus::Ok ||
		reader.read_u32(metadata.sample_count) != WtBinaryStatus::Ok ||
		reader.read_u64(metadata.cell_spacing) != WtBinaryStatus::Ok ||
		reader.read_u64(metadata.source_revision) != WtBinaryStatus::Ok) {
		return WtChunkPageStatus::InvalidMetadata;
	}
	if (major != kWtChunkPageSchemaMajor || minor > kWtChunkPageSchemaMinor ||
		reserved != 0 || sample_minimum != 0xffU || sample_maximum != 17U ||
		reader.remaining() != 0) {
		return WtChunkPageStatus::InvalidMetadata;
	}
	metadata.key.lod = lod;
	metadata.sample_minimum = -1;
	metadata.sample_maximum = 17;
	metadata.density_encoding = static_cast<WtDensityEncoding>(density_encoding);
	metadata.material_encoding = static_cast<WtMaterialEncoding>(material_encoding);
	return valid_metadata(metadata) ?
		WtChunkPageStatus::Ok : WtChunkPageStatus::InvalidMetadata;
}

} // namespace

WtChunkPageStatus wt_write_chunk_page(
	const WtChunkPage &page,
	std::vector<std::uint8_t> &output
) {
	output.clear();
	if (!valid_metadata(page.metadata) ||
		page.samples.size() != kWtChunkPageSampleCount) {
		return WtChunkPageStatus::InvalidInput;
	}
	std::vector<std::uint8_t> header;
	WtChunkPageStatus status = encode_header(page.metadata, header);
	if (status != WtChunkPageStatus::Ok) {
		return status;
	}
	WtBinaryWriter sample_writer(
		kWtChunkPageSampleCount * kWtChunkPageSampleBytes
	);
	for (const WtScalarSample &sample : page.samples) {
		if (!std::isfinite(sample.density)) {
			return WtChunkPageStatus::InvalidSample;
		}
		if (sample_writer.write_f32(sample.density) != WtBinaryStatus::Ok ||
			sample_writer.write_u16(sample.material) != WtBinaryStatus::Ok) {
			return WtChunkPageStatus::CapacityExceeded;
		}
	}
	const std::vector<std::uint8_t> &encoded_samples = sample_writer.bytes();
	const std::vector<WtContainerSectionInput> sections = {
		{ kWtChunkHeaderSection, 0, WtStorageCodec::None,
			{ header.data(), header.size() } },
		{ kWtChunkDataSection, 0, WtStorageCodec::None,
			{ encoded_samples.data(), encoded_samples.size() } },
	};
	return wt_write_container(
		kWtChunkMagic,
		0,
		page.metadata.source_revision,
		sections,
		output
	) == WtContainerStatus::Ok ?
		WtChunkPageStatus::Ok : WtChunkPageStatus::ContainerFailure;
}

WtChunkPageStatus wt_open_chunk_page(
	WtByteView bytes,
	WtChunkPageView &output
) {
	output = {};
	if (wt_read_container(bytes, kWtChunkMagic, output.container) !=
		WtContainerStatus::Ok) {
		return WtChunkPageStatus::ContainerFailure;
	}
	if (output.container.sections.size() != 2) {
		output = {};
		return WtChunkPageStatus::InvalidMetadata;
	}
	const WtContainerSection *header =
		output.container.find_section(kWtChunkHeaderSection);
	const WtContainerSection *data =
		output.container.find_section(kWtChunkDataSection);
	if (header == nullptr || data == nullptr ||
		header->flags != 0 || data->flags != 0 ||
		decode_header(header->payload, output.metadata) != WtChunkPageStatus::Ok ||
		output.metadata.source_revision != output.container.header.source_revision ||
		data->payload.size != kWtChunkPageSampleCount * kWtChunkPageSampleBytes) {
		output = {};
		return WtChunkPageStatus::InvalidMetadata;
	}
	output.encoded_samples = data->payload;
	return WtChunkPageStatus::Ok;
}

WtChunkPageStatus wt_decode_chunk_page(
	const WtChunkPageView &view,
	WtChunkPage &output
) {
	output = {};
	if (!valid_metadata(view.metadata) ||
		view.encoded_samples.size !=
			kWtChunkPageSampleCount * kWtChunkPageSampleBytes) {
		return WtChunkPageStatus::InvalidMetadata;
	}
	output.metadata = view.metadata;
	output.samples.reserve(kWtChunkPageSampleCount);
	WtBinaryReader reader(view.encoded_samples);
	for (std::size_t index = 0; index < kWtChunkPageSampleCount; ++index) {
		WtScalarSample sample;
		if (reader.read_f32(sample.density) != WtBinaryStatus::Ok ||
			reader.read_u16(sample.material) != WtBinaryStatus::Ok ||
			!std::isfinite(sample.density)) {
			output = {};
			return WtChunkPageStatus::InvalidSample;
		}
		output.samples.push_back(sample);
	}
	return reader.remaining() == 0 ?
		WtChunkPageStatus::Ok : WtChunkPageStatus::InvalidMetadata;
}

bool wt_sample_chunk_page(
	const WtChunkPage &page,
	const WtGridPoint &point,
	WtScalarSample &output
) noexcept {
	if (!valid_metadata(page.metadata) ||
		page.samples.size() != kWtChunkPageSampleCount) {
		return false;
	}
	const std::int64_t spacing =
		static_cast<std::int64_t>(page.metadata.cell_spacing);
	const WtGridPoint minimum = wt_chunk_bounds(page.metadata.key).minimum;
	const std::int64_t difference[3] = {
		point.x - minimum.x,
		point.y - minimum.y,
		point.z - minimum.z,
	};
	std::int64_t coordinate[3]{};
	for (unsigned int axis = 0; axis < 3; ++axis) {
		if ((difference[axis] % spacing) != 0) return false;
		coordinate[axis] = difference[axis] / spacing;
		if (coordinate[axis] < page.metadata.sample_minimum ||
			coordinate[axis] > page.metadata.sample_maximum) {
			return false;
		}
	}
	const std::size_t dimension = page.metadata.dimension_x;
	const std::size_t index = static_cast<std::size_t>(
		((coordinate[2] - page.metadata.sample_minimum) * dimension +
			(coordinate[1] - page.metadata.sample_minimum)) * dimension +
		(coordinate[0] - page.metadata.sample_minimum)
	);
	if (index >= page.samples.size()) return false;
	output = page.samples[index];
	return true;
}

} // namespace world_transvoxel
