#pragma once

#include <array>
#include <cstdint>

namespace world_transvoxel {

constexpr unsigned int kWtRegularSampleCount = 8;
constexpr unsigned int kWtTransitionSampleCount = 9;
constexpr unsigned int kWtTransitionTopologySampleCount = 13;
constexpr unsigned int kWtCellMaxVertexCount = 12;
constexpr unsigned int kWtCellMaxIndexCount = 36;

struct WtVec3 {
	float x = 0.0F;
	float y = 0.0F;
	float z = 0.0F;
};

struct WtCellSample {
	float density = 0.0F;
	WtVec3 gradient;
	std::uint16_t material = 0;
	bool material_authored = false;
};

struct WtCellVertex {
	WtVec3 position;
	WtVec3 normal;
	std::uint16_t material = 0;
	bool material_authored = false;
	std::uint8_t endpoint_a = 0;
	std::uint8_t endpoint_b = 0;
};

struct WtCellMesh {
	std::array<WtCellVertex, kWtCellMaxVertexCount> vertices{};
	std::array<std::uint8_t, kWtCellMaxIndexCount> indices{};
	std::uint8_t vertex_count = 0;
	std::uint8_t index_count = 0;

	void clear() noexcept {
		vertex_count = 0;
		index_count = 0;
	}
};

enum class WtCellStatus : std::uint8_t {
	Ok,
	Empty,
	NonFiniteInput,
	InvalidScale,
	InvalidOrientation,
	TopologyFailure,
};

// Direction from the full-resolution face toward the half-resolution face.
enum class WtTransitionOrientation : std::uint8_t {
	PositiveX,
	NegativeX,
	PositiveY,
	NegativeY,
	PositiveZ,
	NegativeZ,
};

struct WtRegularCellInput {
	std::array<WtCellSample, kWtRegularSampleCount> samples{};
	float isovalue = 0.0F;
	WtVec3 origin;
	float cell_size = 1.0F;
};

struct WtTransitionCellInput {
	std::array<WtCellSample, kWtTransitionSampleCount> samples{};
	float isovalue = 0.0F;
	WtVec3 full_resolution_origin;
	float sample_spacing = 1.0F;
	float transition_width = 0.25F;
	WtTransitionOrientation orientation = WtTransitionOrientation::PositiveZ;
};

struct WtCellMeshingScratch {
	std::array<WtCellSample, kWtTransitionTopologySampleCount> samples{};
	std::array<WtVec3, kWtTransitionTopologySampleCount> positions{};
	std::array<WtCellVertex, kWtCellMaxVertexCount> vertices{};
	std::array<std::uint8_t, kWtCellMaxIndexCount> indices{};
	std::uint8_t vertex_count = 0;
	std::uint8_t index_count = 0;

	void clear() noexcept {
		vertex_count = 0;
		index_count = 0;
	}
};

WtCellMeshingScratch &wt_get_thread_cell_meshing_scratch() noexcept;

} // namespace world_transvoxel
