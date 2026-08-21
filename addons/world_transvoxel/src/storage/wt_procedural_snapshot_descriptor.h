#pragma once

#include "storage/wt_container_format.h"
#include "storage/wt_procedural_world_descriptor.h"

#include <cstdint>
#include <vector>

namespace world_transvoxel {

constexpr std::uint16_t kWtProceduralSnapshotSchemaMajor = 1;
constexpr std::uint16_t kWtProceduralSnapshotSchemaMinor = 1;
constexpr std::size_t kWtMaximumProceduralOverlayPageCount = 65536;
constexpr std::uint32_t kWtProceduralSnapshotMetadataSection =
	wt_fourcc('P', 'R', 'O', 'C');
struct WtProceduralSnapshotDescriptor {
	WtProceduralWorldDescriptor world;
	WtHash256 overlay_manifest_hash{};
};

enum class WtProceduralSnapshotDescriptorStatus : std::uint8_t {
	Ok,
	InvalidInput,
	InvalidDescriptor,
	ContainerFailure,
};

WtHash256 wt_procedural_configuration_hash(
	const WtProceduralWorldDescriptor &descriptor
);

WtProceduralSnapshotDescriptorStatus wt_write_procedural_snapshot_descriptor(
	const WtProceduralSnapshotDescriptor &descriptor,
	std::vector<std::uint8_t> &output
);

WtProceduralSnapshotDescriptorStatus wt_open_procedural_snapshot_descriptor(
	WtByteView bytes,
	WtProceduralSnapshotDescriptor &output
);

} // namespace world_transvoxel
