#include "backend/wt_transvoxel_mit_backend.h"

#include <cmath>

// The upstream file is deliberately included in this MIT adapter translation
// unit so official table types and symbols do not leak into project headers.
#include "../../thirdparty/transvoxel_mit/Transvoxel.cpp"

namespace world_transvoxel {
namespace {

static_assert(sizeof(regularCellClass) == 256);
static_assert(sizeof(transitionCellClass) == 512);
static_assert(sizeof(regularCellData) / sizeof(regularCellData[0]) == 16);
static_assert(sizeof(transitionCellData) / sizeof(transitionCellData[0]) == 56);

constexpr WtMeshingBackendInfo kBackendInfo = {
	"transvoxel_mit_official",
	"MIT",
	"51a494f03c5b024cd153b596bcc7152eb3cc93a6",
	256,
	512,
	true,
};

const WtTransvoxelMitBackend kBackend;

bool is_finite(float value) noexcept {
	return std::isfinite(value);
}

bool is_finite(const WtVec3 &value) noexcept {
	return is_finite(value.x) && is_finite(value.y) && is_finite(value.z);
}

WtVec3 add(const WtVec3 &a, const WtVec3 &b) noexcept {
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}

WtVec3 subtract(const WtVec3 &a, const WtVec3 &b) noexcept {
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

WtVec3 multiply(const WtVec3 &value, float scalar) noexcept {
	return { value.x * scalar, value.y * scalar, value.z * scalar };
}

WtVec3 lerp(const WtVec3 &a, const WtVec3 &b, float alpha) noexcept {
	return add(multiply(a, 1.0F - alpha), multiply(b, alpha));
}

WtVec3 cross(const WtVec3 &a, const WtVec3 &b) noexcept {
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x,
	};
}

float length_squared(const WtVec3 &value) noexcept {
	return value.x * value.x + value.y * value.y + value.z * value.z;
}

WtVec3 normalized(const WtVec3 &value) noexcept {
	const float squared_length = length_squared(value);
	if (squared_length == 0.0F) {
		return {};
	}
	return multiply(value, 1.0F / std::sqrt(squared_length));
}

template <std::size_t SampleCount>
WtCellStatus validate_samples(
	const std::array<WtCellSample, SampleCount> &samples,
	float isovalue,
	const WtVec3 &origin
) noexcept {
	if (!is_finite(isovalue) || !is_finite(origin)) {
		return WtCellStatus::NonFiniteInput;
	}
	for (const WtCellSample &sample : samples) {
		if (!is_finite(sample.density) || !is_finite(sample.gradient)) {
			return WtCellStatus::NonFiniteInput;
		}
	}
	return WtCellStatus::Ok;
}

WtCellStatus make_vertex(
	std::uint8_t endpoint_a,
	std::uint8_t endpoint_b,
	float isovalue,
	const WtCellMeshingScratch &scratch,
	WtCellVertex &vertex
) noexcept {
	const WtCellSample &sample_a = scratch.samples[endpoint_a];
	const WtCellSample &sample_b = scratch.samples[endpoint_b];
	const float denominator = sample_b.density - sample_a.density;
	if (denominator == 0.0F) {
		return WtCellStatus::TopologyFailure;
	}
	float alpha = (isovalue - sample_a.density) / denominator;
	if (alpha < 0.0F) {
		alpha = 0.0F;
	} else if (alpha > 1.0F) {
		alpha = 1.0F;
	}

	vertex.position = lerp(
		scratch.positions[endpoint_a],
		scratch.positions[endpoint_b],
		alpha
	);
	vertex.normal = normalized(lerp(sample_a.gradient, sample_b.gradient, alpha));
	vertex.material = sample_a.density < isovalue ? sample_a.material : sample_b.material;
	vertex.endpoint_a = endpoint_a;
	vertex.endpoint_b = endpoint_b;
	return WtCellStatus::Ok;
}

void remove_degenerate_triangles(WtCellMeshingScratch &scratch) noexcept {
	constexpr float kMinimumTriangleEdgeLengthSquared = 0.000001F;
	std::uint8_t write_index = 0;
	for (std::uint8_t read_index = 0; read_index < scratch.index_count; read_index += 3) {
		const std::uint8_t i0 = scratch.indices[read_index];
		const std::uint8_t i1 = scratch.indices[read_index + 1];
		const std::uint8_t i2 = scratch.indices[read_index + 2];
		const WtVec3 edge_a = subtract(
			scratch.vertices[i1].position,
			scratch.vertices[i0].position
		);
		const WtVec3 edge_b = subtract(
			scratch.vertices[i2].position,
			scratch.vertices[i0].position
		);
		const WtVec3 edge_c = subtract(
			scratch.vertices[i2].position,
			scratch.vertices[i1].position
		);
		if (length_squared(edge_a) <= kMinimumTriangleEdgeLengthSquared ||
			length_squared(edge_b) <= kMinimumTriangleEdgeLengthSquared ||
			length_squared(edge_c) <= kMinimumTriangleEdgeLengthSquared) {
			continue;
		}
		if (length_squared(cross(edge_a, edge_b)) == 0.0F) {
			continue;
		}
		scratch.indices[write_index++] = i0;
		scratch.indices[write_index++] = i1;
		scratch.indices[write_index++] = i2;
	}
	scratch.index_count = write_index;
}

WtCellStatus finish_mesh(WtCellMeshingScratch &scratch, WtCellMesh &output) noexcept {
	remove_degenerate_triangles(scratch);
	if (scratch.index_count == 0) {
		output.clear();
		return WtCellStatus::Empty;
	}
	output.vertex_count = scratch.vertex_count;
	output.index_count = scratch.index_count;
	for (std::uint8_t index = 0; index < scratch.vertex_count; ++index) {
		output.vertices[index] = scratch.vertices[index];
	}
	for (std::uint8_t index = 0; index < scratch.index_count; ++index) {
		output.indices[index] = scratch.indices[index];
	}
	return WtCellStatus::Ok;
}

bool get_transition_basis(
	WtTransitionOrientation orientation,
	WtVec3 &axis_u,
	WtVec3 &axis_v,
	WtVec3 &axis_w
) noexcept {
	switch (orientation) {
		case WtTransitionOrientation::PositiveX:
			axis_u = { 0.0F, 1.0F, 0.0F };
			axis_v = { 0.0F, 0.0F, 1.0F };
			axis_w = { 1.0F, 0.0F, 0.0F };
			return true;
		case WtTransitionOrientation::NegativeX:
			axis_u = { 0.0F, 1.0F, 0.0F };
			axis_v = { 0.0F, 0.0F, -1.0F };
			axis_w = { -1.0F, 0.0F, 0.0F };
			return true;
		case WtTransitionOrientation::PositiveY:
			axis_u = { 0.0F, 0.0F, 1.0F };
			axis_v = { 1.0F, 0.0F, 0.0F };
			axis_w = { 0.0F, 1.0F, 0.0F };
			return true;
		case WtTransitionOrientation::NegativeY:
			axis_u = { 0.0F, 0.0F, 1.0F };
			axis_v = { -1.0F, 0.0F, 0.0F };
			axis_w = { 0.0F, -1.0F, 0.0F };
			return true;
		case WtTransitionOrientation::PositiveZ:
			axis_u = { 1.0F, 0.0F, 0.0F };
			axis_v = { 0.0F, 1.0F, 0.0F };
			axis_w = { 0.0F, 0.0F, 1.0F };
			return true;
		case WtTransitionOrientation::NegativeZ:
			axis_u = { 1.0F, 0.0F, 0.0F };
			axis_v = { 0.0F, -1.0F, 0.0F };
			axis_w = { 0.0F, 0.0F, -1.0F };
			return true;
	}
	return false;
}

WtVec3 transition_position(
	const WtTransitionCellInput &input,
	const WtVec3 &axis_u,
	const WtVec3 &axis_v,
	const WtVec3 &axis_w,
	float u,
	float v,
	float w
) noexcept {
	return add(
		input.full_resolution_origin,
		add(
			multiply(axis_u, u * input.sample_spacing),
			add(
				multiply(axis_v, v * input.sample_spacing),
				multiply(axis_w, w * input.transition_width)
			)
		)
	);
}

} // namespace

const WtMeshingBackendInfo &WtTransvoxelMitBackend::get_info() const noexcept {
	return kBackendInfo;
}

bool WtTransvoxelMitBackend::is_available() const noexcept {
	return true;
}

WtCellStatus WtTransvoxelMitBackend::mesh_regular_cell(
	const WtRegularCellInput &input,
	WtCellMesh &output,
	WtCellMeshingScratch &scratch
) const noexcept {
	output.clear();
	scratch.clear();
	const WtCellStatus validation = validate_samples(
		input.samples,
		input.isovalue,
		input.origin
	);
	if (validation != WtCellStatus::Ok) {
		return validation;
	}
	if (!is_finite(input.cell_size) || input.cell_size <= 0.0F) {
		return WtCellStatus::InvalidScale;
	}

	std::uint8_t case_code = 0;
	for (std::uint8_t index = 0; index < kWtRegularSampleCount; ++index) {
		scratch.samples[index] = input.samples[index];
		scratch.positions[index] = add(
			input.origin,
			{
				(index & 1U) != 0U ? input.cell_size : 0.0F,
				(index & 2U) != 0U ? input.cell_size : 0.0F,
				(index & 4U) != 0U ? input.cell_size : 0.0F,
			}
		);
		if (input.samples[index].density < input.isovalue) {
			case_code |= static_cast<std::uint8_t>(1U << index);
		}
	}
	if (case_code == 0 || case_code == 255) {
		return WtCellStatus::Empty;
	}

	const RegularCellData &cell_data = regularCellData[regularCellClass[case_code]];
	scratch.vertex_count = static_cast<std::uint8_t>(cell_data.GetVertexCount());
	scratch.index_count = static_cast<std::uint8_t>(cell_data.GetTriangleCount() * 3);
	if (scratch.vertex_count > kWtCellMaxVertexCount ||
		scratch.index_count > kWtCellMaxIndexCount) {
		return WtCellStatus::TopologyFailure;
	}
	for (std::uint8_t index = 0; index < scratch.vertex_count; ++index) {
		const unsigned short edge_code = regularVertexData[case_code][index];
		const std::uint8_t endpoint_a = static_cast<std::uint8_t>((edge_code >> 4) & 0x0F);
		const std::uint8_t endpoint_b = static_cast<std::uint8_t>(edge_code & 0x0F);
		if (endpoint_a >= kWtRegularSampleCount || endpoint_b >= kWtRegularSampleCount) {
			return WtCellStatus::TopologyFailure;
		}
		const WtCellStatus status = make_vertex(
			endpoint_a,
			endpoint_b,
			input.isovalue,
			scratch,
			scratch.vertices[index]
		);
		if (status != WtCellStatus::Ok) {
			return status;
		}
	}
	for (std::uint8_t index = 0; index < scratch.index_count; ++index) {
		const std::uint8_t vertex_index = cell_data.vertexIndex[index];
		if (vertex_index >= scratch.vertex_count) {
			return WtCellStatus::TopologyFailure;
		}
		scratch.indices[index] = vertex_index;
	}
	return finish_mesh(scratch, output);
}

WtCellStatus WtTransvoxelMitBackend::mesh_transition_cell(
	const WtTransitionCellInput &input,
	WtCellMesh &output,
	WtCellMeshingScratch &scratch
) const noexcept {
	output.clear();
	scratch.clear();
	const WtCellStatus validation = validate_samples(
		input.samples,
		input.isovalue,
		input.full_resolution_origin
	);
	if (validation != WtCellStatus::Ok) {
		return validation;
	}
	if (!is_finite(input.sample_spacing) || !is_finite(input.transition_width) ||
		input.sample_spacing <= 0.0F || input.transition_width <= 0.0F) {
		return WtCellStatus::InvalidScale;
	}

	WtVec3 axis_u;
	WtVec3 axis_v;
	WtVec3 axis_w;
	if (!get_transition_basis(input.orientation, axis_u, axis_v, axis_w)) {
		return WtCellStatus::InvalidOrientation;
	}

	for (std::uint8_t index = 0; index < kWtTransitionSampleCount; ++index) {
		scratch.samples[index] = input.samples[index];
		const float u = static_cast<float>(index % 3U);
		const float v = static_cast<float>(index / 3U);
		scratch.positions[index] = transition_position(
			input,
			axis_u,
			axis_v,
			axis_w,
			u,
			v,
			0.0F
		);
	}
	constexpr std::array<std::uint8_t, 4> kHalfResolutionAliases = { 0, 2, 6, 8 };
	constexpr std::array<WtVec3, 4> kHalfResolutionCoordinates = {
		WtVec3{ 0.0F, 0.0F, 1.0F },
		WtVec3{ 2.0F, 0.0F, 1.0F },
		WtVec3{ 0.0F, 2.0F, 1.0F },
		WtVec3{ 2.0F, 2.0F, 1.0F },
	};
	for (std::uint8_t index = 0; index < 4; ++index) {
		const std::uint8_t topology_index = static_cast<std::uint8_t>(9 + index);
		scratch.samples[topology_index] = input.samples[kHalfResolutionAliases[index]];
		const WtVec3 coordinate = kHalfResolutionCoordinates[index];
		scratch.positions[topology_index] = transition_position(
			input,
			axis_u,
			axis_v,
			axis_w,
			coordinate.x,
			coordinate.y,
			coordinate.z
		);
	}

	constexpr std::array<std::uint8_t, 9> kCaseBitSamples = { 0, 1, 2, 5, 8, 7, 6, 3, 4 };
	std::uint16_t case_code = 0;
	for (std::uint8_t bit = 0; bit < kCaseBitSamples.size(); ++bit) {
		if (input.samples[kCaseBitSamples[bit]].density < input.isovalue) {
			case_code |= static_cast<std::uint16_t>(1U << bit);
		}
	}
	if (case_code == 0 || case_code == 511) {
		return WtCellStatus::Empty;
	}

	const std::uint8_t class_code = transitionCellClass[case_code];
	const bool reverse_winding = (class_code & 0x80U) != 0U;
	const std::uint8_t class_index = class_code & 0x7FU;
	if (class_index >= 56) {
		return WtCellStatus::TopologyFailure;
	}
	const TransitionCellData &cell_data = transitionCellData[class_index];
	scratch.vertex_count = static_cast<std::uint8_t>(cell_data.GetVertexCount());
	scratch.index_count = static_cast<std::uint8_t>(cell_data.GetTriangleCount() * 3);
	if (scratch.vertex_count > kWtCellMaxVertexCount ||
		scratch.index_count > kWtCellMaxIndexCount) {
		return WtCellStatus::TopologyFailure;
	}
	for (std::uint8_t index = 0; index < scratch.vertex_count; ++index) {
		const unsigned short edge_code = transitionVertexData[case_code][index];
		const std::uint8_t endpoint_a = static_cast<std::uint8_t>((edge_code >> 4) & 0x0F);
		const std::uint8_t endpoint_b = static_cast<std::uint8_t>(edge_code & 0x0F);
		if (endpoint_a >= kWtTransitionTopologySampleCount ||
			endpoint_b >= kWtTransitionTopologySampleCount) {
			return WtCellStatus::TopologyFailure;
		}
		const WtCellStatus status = make_vertex(
			endpoint_a,
			endpoint_b,
			input.isovalue,
			scratch,
			scratch.vertices[index]
		);
		if (status != WtCellStatus::Ok) {
			return status;
		}
	}
	for (std::uint8_t index = 0; index < scratch.index_count; index += 3) {
		const std::uint8_t first = cell_data.vertexIndex[index];
		const std::uint8_t second = cell_data.vertexIndex[index + 1];
		const std::uint8_t third = cell_data.vertexIndex[index + 2];
		if (first >= scratch.vertex_count || second >= scratch.vertex_count ||
			third >= scratch.vertex_count) {
			return WtCellStatus::TopologyFailure;
		}
		scratch.indices[index] = reverse_winding ? third : first;
		scratch.indices[index + 1] = second;
		scratch.indices[index + 2] = reverse_winding ? first : third;
	}
	return finish_mesh(scratch, output);
}

const WtMeshingBackend &wt_get_transvoxel_mit_backend() noexcept {
	return kBackend;
}

} // namespace world_transvoxel
