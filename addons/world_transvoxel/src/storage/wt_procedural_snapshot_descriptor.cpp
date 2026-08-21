#include "storage/wt_procedural_snapshot_descriptor.h"

#include "storage/wt_binary_io.h"
#include "storage/wt_procedural_world_source.h"

#include <algorithm>

namespace world_transvoxel {
namespace {

constexpr std::size_t kMetadataSize = 76;

bool encode_geometry(
	const WtProceduralWorldDescriptor &descriptor,
	WtBinaryWriter &writer
) {
	return writer.write_u32(descriptor.chunk_count_x) == WtBinaryStatus::Ok &&
		writer.write_u32(descriptor.chunk_count_y) == WtBinaryStatus::Ok &&
		writer.write_u32(descriptor.chunk_count_z) == WtBinaryStatus::Ok &&
		writer.write_i32(descriptor.chunk_y) == WtBinaryStatus::Ok &&
		writer.write_u32(descriptor.seed) == WtBinaryStatus::Ok &&
		writer.write_u8(static_cast<std::uint8_t>(descriptor.mode)) ==
			WtBinaryStatus::Ok;
}

} // namespace

WtHash256 wt_procedural_configuration_hash(
	const WtProceduralWorldDescriptor &descriptor
) {
	WtBinaryWriter writer(32);
	if (!encode_geometry(descriptor, writer)) return {};
	if (descriptor.bottom_boundary_policy !=
			WtProceduralBottomBoundaryPolicy::Open &&
		(writer.write_u8(static_cast<std::uint8_t>(
			descriptor.bottom_boundary_policy
		)) != WtBinaryStatus::Ok ||
		writer.write_u16(descriptor.bottom_boundary_thickness_cells) !=
			WtBinaryStatus::Ok)) {
		return {};
	}
	const std::vector<std::uint8_t> bytes = writer.take_bytes();
	return wt_sha256(bytes.data(), bytes.size());
}

WtProceduralSnapshotDescriptorStatus wt_write_procedural_snapshot_descriptor(
	const WtProceduralSnapshotDescriptor &descriptor,
	std::vector<std::uint8_t> &output
) {
	output.clear();
	if (!wt_valid_procedural_descriptor(descriptor.world) ||
		wt_is_zero_hash(descriptor.overlay_manifest_hash)) {
		return WtProceduralSnapshotDescriptorStatus::InvalidInput;
	}
	WtBinaryWriter writer(kMetadataSize);
	const std::uint16_t schema_minor =
		descriptor.world.bottom_boundary_policy ==
			WtProceduralBottomBoundaryPolicy::Open ?
			0 : kWtProceduralSnapshotSchemaMinor;
	if (writer.write_u16(kWtProceduralSnapshotSchemaMajor) !=
			WtBinaryStatus::Ok ||
		writer.write_u16(schema_minor) !=
			WtBinaryStatus::Ok ||
		!encode_geometry(descriptor.world, writer) ||
		writer.write_u8(static_cast<std::uint8_t>(
			descriptor.world.bottom_boundary_policy
		)) != WtBinaryStatus::Ok ||
		writer.write_u16(
			descriptor.world.bottom_boundary_thickness_cells
		) != WtBinaryStatus::Ok ||
		writer.write_u64(descriptor.world.source_revision) !=
			WtBinaryStatus::Ok ||
		writer.write_u64(descriptor.world.world_revision) !=
			WtBinaryStatus::Ok ||
		writer.write_bytes(
			descriptor.overlay_manifest_hash.data(),
			descriptor.overlay_manifest_hash.size()
		) != WtBinaryStatus::Ok ||
		writer.bytes().size() != kMetadataSize) {
		return WtProceduralSnapshotDescriptorStatus::InvalidInput;
	}
	const std::vector<std::uint8_t> metadata = writer.take_bytes();
	const std::vector<WtContainerSectionInput> sections = {
		{ kWtProceduralSnapshotMetadataSection, 0, WtStorageCodec::None,
			{ metadata.data(), metadata.size() } },
	};
	return wt_write_container(
		kWtProceduralSnapshotMagic,
		0,
		descriptor.world.source_revision,
		sections,
		output
	) == WtContainerStatus::Ok ?
		WtProceduralSnapshotDescriptorStatus::Ok :
		WtProceduralSnapshotDescriptorStatus::ContainerFailure;
}

WtProceduralSnapshotDescriptorStatus wt_open_procedural_snapshot_descriptor(
	WtByteView bytes,
	WtProceduralSnapshotDescriptor &output
) {
	output = {};
	WtContainerView container;
	if (wt_read_container(bytes, kWtProceduralSnapshotMagic, container) !=
			WtContainerStatus::Ok) {
		return WtProceduralSnapshotDescriptorStatus::ContainerFailure;
	}
	const WtContainerSection *metadata =
		container.find_section(kWtProceduralSnapshotMetadataSection);
	if (container.sections.size() != 1 || metadata == nullptr ||
		metadata->flags != 0 || metadata->payload.size != kMetadataSize) {
		return WtProceduralSnapshotDescriptorStatus::InvalidDescriptor;
	}
	WtBinaryReader reader(metadata->payload);
	std::uint16_t major = 0;
	std::uint16_t minor = 0;
	std::uint8_t mode = 0;
	std::uint8_t boundary_policy = 0;
	std::uint16_t boundary_thickness = 0;
	WtByteView manifest_hash;
	if (reader.read_u16(major) != WtBinaryStatus::Ok ||
		reader.read_u16(minor) != WtBinaryStatus::Ok ||
		reader.read_u32(output.world.chunk_count_x) != WtBinaryStatus::Ok ||
		reader.read_u32(output.world.chunk_count_y) != WtBinaryStatus::Ok ||
		reader.read_u32(output.world.chunk_count_z) != WtBinaryStatus::Ok ||
		reader.read_i32(output.world.chunk_y) != WtBinaryStatus::Ok ||
		reader.read_u32(output.world.seed) != WtBinaryStatus::Ok ||
		reader.read_u8(mode) != WtBinaryStatus::Ok ||
		reader.read_u8(boundary_policy) != WtBinaryStatus::Ok ||
		reader.read_u16(boundary_thickness) != WtBinaryStatus::Ok ||
		reader.read_u64(output.world.source_revision) != WtBinaryStatus::Ok ||
		reader.read_u64(output.world.world_revision) != WtBinaryStatus::Ok ||
		reader.read_bytes(output.overlay_manifest_hash.size(), manifest_hash) !=
			WtBinaryStatus::Ok ||
		major != kWtProceduralSnapshotSchemaMajor ||
		minor > kWtProceduralSnapshotSchemaMinor ||
		(minor == 0 && (boundary_policy != 0 || boundary_thickness != 0)) ||
		reader.remaining() != 0) {
		output = {};
		return WtProceduralSnapshotDescriptorStatus::InvalidDescriptor;
	}
	output.world.mode = static_cast<WtProceduralWorldMode>(mode);
	output.world.bottom_boundary_policy =
		static_cast<WtProceduralBottomBoundaryPolicy>(boundary_policy);
	output.world.bottom_boundary_thickness_cells = boundary_thickness;
	std::copy(
		manifest_hash.data,
		manifest_hash.data + manifest_hash.size,
		output.overlay_manifest_hash.begin()
	);
	if (container.header.source_revision != output.world.source_revision ||
		!wt_valid_procedural_descriptor(output.world) ||
		wt_is_zero_hash(output.overlay_manifest_hash)) {
		output = {};
		return WtProceduralSnapshotDescriptorStatus::InvalidDescriptor;
	}
	return WtProceduralSnapshotDescriptorStatus::Ok;
}

} // namespace world_transvoxel
