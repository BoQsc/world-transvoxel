#pragma once

#include "core/wt_meshing_limits.h"
#include "meshing/wt_chunk_mesher.h"
#include "storage/wt_container_format.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

constexpr std::uint16_t kWtChunkPageSchemaMajor = 1;
constexpr std::uint16_t kWtChunkPageSchemaMinor = 1;
constexpr std::size_t kWtChunkPageHeaderSize = 48;
constexpr std::size_t kWtChunkPageSampleCount =
	kWtChunkMeshingSamplesPerAxis *
	kWtChunkMeshingSamplesPerAxis *
	kWtChunkMeshingSamplesPerAxis;
constexpr std::size_t kWtChunkPageSampleBytes = 6;
constexpr std::size_t kWtChunkSurfaceShiftHeaderBytes = 8;
constexpr std::size_t kWtChunkSurfaceShiftRecordBytes = 42;
constexpr std::size_t kWtChunkSurfaceEdgeCount =
	3 * kWtChunkCellsPerAxis *
	(kWtChunkCellsPerAxis + 1) *
	(kWtChunkCellsPerAxis + 1);
constexpr std::uint32_t kWtChunkHeaderSection = wt_fourcc('C', 'H', 'D', 'R');
constexpr std::uint32_t kWtChunkDataSection = wt_fourcc('D', 'A', 'T', 'A');
constexpr std::uint32_t kWtChunkSurfaceShiftSection = wt_fourcc('S', 'H', 'F', 'T');

enum class WtDensityEncoding : std::uint8_t {
	Float32 = 1,
};

enum class WtMaterialEncoding : std::uint8_t {
	Uint16 = 1,
};

struct WtChunkPageMetadata {
	std::uint16_t schema_minor = kWtChunkPageSchemaMinor;
	WtChunkKey key;
	std::int8_t sample_minimum = -1;
	std::int8_t sample_maximum = 17;
	std::uint16_t dimension_x = kWtChunkMeshingSamplesPerAxis;
	std::uint16_t dimension_y = kWtChunkMeshingSamplesPerAxis;
	std::uint16_t dimension_z = kWtChunkMeshingSamplesPerAxis;
	WtDensityEncoding density_encoding = WtDensityEncoding::Float32;
	WtMaterialEncoding material_encoding = WtMaterialEncoding::Uint16;
	std::uint32_t sample_count = kWtChunkPageSampleCount;
	std::uint64_t cell_spacing = 1;
	std::uint64_t source_revision = 0;
};

struct WtChunkSurfaceShiftRecord {
	std::uint16_t edge_index = 0;
	std::uint32_t unit_offset = 0;
	WtCellSample sample_a;
	WtCellSample sample_b;
};

struct WtChunkPage {
	WtChunkPageMetadata metadata;
	std::vector<WtScalarSample> samples;
	float surface_shift_isovalue = 0.0F;
	std::vector<WtChunkSurfaceShiftRecord> surface_shift_records;
	bool surface_shift_valid = true;
};

struct WtChunkPageView {
	WtContainerView container;
	WtChunkPageMetadata metadata;
	WtByteView encoded_samples;
	WtByteView encoded_surface_shift;
};

enum class WtChunkPageStatus : std::uint8_t {
	Ok,
	InvalidInput,
	InvalidMetadata,
	InvalidSample,
	CapacityExceeded,
	ContainerFailure,
};

WtChunkPageStatus wt_write_chunk_page(
	const WtChunkPage &page,
	std::vector<std::uint8_t> &output
);

WtChunkPageStatus wt_open_chunk_page(
	WtByteView bytes,
	WtChunkPageView &output
);

WtChunkPageStatus wt_decode_chunk_page(
	const WtChunkPageView &view,
	WtChunkPage &output
);

bool wt_sample_chunk_page(
	const WtChunkPage &page,
	const WtGridPoint &point,
	WtScalarSample &output
) noexcept;

} // namespace world_transvoxel
