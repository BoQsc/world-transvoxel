#include "backend/wt_transvoxel_mit_backend.h"
#include "core/wt_meshing_limits.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace wt = world_transvoxel;

namespace {

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

bool nearly_equal(float a, float b, float tolerance = 0.00001F) {
	return std::fabs(a - b) <= tolerance;
}

wt::WtVec3 subtract(const wt::WtVec3 &a, const wt::WtVec3 &b) {
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

wt::WtVec3 cross(const wt::WtVec3 &a, const wt::WtVec3 &b) {
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x,
	};
}

float dot(const wt::WtVec3 &a, const wt::WtVec3 &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool equal_vec(const wt::WtVec3 &a, const wt::WtVec3 &b) {
	return nearly_equal(a.x, b.x) && nearly_equal(a.y, b.y) && nearly_equal(a.z, b.z);
}

bool equal_mesh(const wt::WtCellMesh &a, const wt::WtCellMesh &b) {
	if (a.vertex_count != b.vertex_count || a.index_count != b.index_count) {
		return false;
	}
	for (std::uint8_t index = 0; index < a.vertex_count; ++index) {
		if (!equal_vec(a.vertices[index].position, b.vertices[index].position) ||
			!equal_vec(a.vertices[index].normal, b.vertices[index].normal) ||
			a.vertices[index].material != b.vertices[index].material) {
			return false;
		}
	}
	for (std::uint8_t index = 0; index < a.index_count; ++index) {
		if (a.indices[index] != b.indices[index]) {
			return false;
		}
	}
	return true;
}

void hash_byte(std::uint64_t &hash, std::uint8_t value) {
	hash ^= value;
	hash *= 1099511628211ULL;
}

void hash_u16(std::uint64_t &hash, std::uint16_t value) {
	hash_byte(hash, static_cast<std::uint8_t>(value));
	hash_byte(hash, static_cast<std::uint8_t>(value >> 8));
}

void hash_float(std::uint64_t &hash, float value) {
	std::uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&bits, &value, sizeof(bits));
	for (unsigned int shift = 0; shift < 32; shift += 8) {
		hash_byte(hash, static_cast<std::uint8_t>(bits >> shift));
	}
}

void hash_vec(std::uint64_t &hash, const wt::WtVec3 &value) {
	hash_float(hash, value.x);
	hash_float(hash, value.y);
	hash_float(hash, value.z);
}

void hash_mesh(std::uint64_t &hash, wt::WtCellStatus status, const wt::WtCellMesh &mesh) {
	hash_byte(hash, static_cast<std::uint8_t>(status));
	hash_byte(hash, mesh.vertex_count);
	hash_byte(hash, mesh.index_count);
	for (std::uint8_t index = 0; index < mesh.vertex_count; ++index) {
		hash_vec(hash, mesh.vertices[index].position);
		hash_vec(hash, mesh.vertices[index].normal);
		hash_u16(hash, mesh.vertices[index].material);
	}
	for (std::uint8_t index = 0; index < mesh.index_count; ++index) {
		hash_byte(hash, mesh.indices[index]);
	}
}

wt::WtRegularCellInput make_regular_input(unsigned int case_code) {
	wt::WtRegularCellInput input;
	for (unsigned int index = 0; index < input.samples.size(); ++index) {
		input.samples[index].density = (case_code & (1U << index)) != 0U ? -1.0F : 1.0F;
		input.samples[index].gradient = { 1.0F, 0.0F, 0.0F };
		input.samples[index].material = static_cast<std::uint16_t>(index + 1);
	}
	return input;
}

constexpr std::uint8_t kTransitionBitSamples[9] = { 0, 1, 2, 5, 8, 7, 6, 3, 4 };

wt::WtTransitionCellInput make_transition_input(unsigned int case_code) {
	wt::WtTransitionCellInput input;
	for (unsigned int index = 0; index < input.samples.size(); ++index) {
		input.samples[index].density = 1.0F;
		input.samples[index].gradient = { 1.0F, 0.0F, 0.0F };
		input.samples[index].material = static_cast<std::uint16_t>(index + 1);
	}
	for (unsigned int bit = 0; bit < 9; ++bit) {
		if ((case_code & (1U << bit)) != 0U) {
			input.samples[kTransitionBitSamples[bit]].density = -1.0F;
		}
	}
	return input;
}

void validate_mesh(
	const wt::WtCellMesh &mesh,
	const wt::WtCellSample *samples,
	unsigned int sample_count,
	unsigned int topology_sample_count
) {
	check(mesh.vertex_count <= wt::kWtCellMaxVertexCount, "vertex capacity exceeded");
	check(mesh.index_count <= wt::kWtCellMaxIndexCount, "index capacity exceeded");
	check((mesh.index_count % 3U) == 0U, "index count is not triangular");
	for (std::uint8_t index = 0; index < mesh.index_count; ++index) {
		check(mesh.indices[index] < mesh.vertex_count, "index is outside active vertices");
	}
	for (std::uint8_t index = 0; index < mesh.vertex_count; ++index) {
		const wt::WtCellVertex &vertex = mesh.vertices[index];
		check(std::isfinite(vertex.position.x) && std::isfinite(vertex.position.y) &&
			std::isfinite(vertex.position.z), "position is not finite");
		check(nearly_equal(dot(vertex.normal, vertex.normal), 1.0F), "normal is not unit length");
		check(vertex.material > 0 && vertex.material <= sample_count, "material is outside sample set");
		check(vertex.endpoint_a < topology_sample_count && vertex.endpoint_b < topology_sample_count,
			"vertex endpoint provenance is outside topology samples");
		check(vertex.endpoint_a != vertex.endpoint_b,
			"vertex endpoint provenance names a zero-length topology edge");
		const unsigned int aliases[4] = { 0, 2, 6, 8 };
		if (vertex.endpoint_a < topology_sample_count &&
			vertex.endpoint_b < topology_sample_count) {
			const unsigned int source_a = vertex.endpoint_a < sample_count ?
				vertex.endpoint_a : aliases[vertex.endpoint_a - 9];
			const unsigned int source_b = vertex.endpoint_b < sample_count ?
				vertex.endpoint_b : aliases[vertex.endpoint_b - 9];
			check((samples[source_a].density < 0.0F) !=
				(samples[source_b].density < 0.0F),
				"vertex endpoint provenance does not cross the isosurface");
		}
		if (vertex.material > 0 && vertex.material <= sample_count) {
			check(samples[vertex.material - 1].density < 0.0F, "material did not come from solid endpoint");
		}
	}
}

void get_basis(
	wt::WtTransitionOrientation orientation,
	wt::WtVec3 &u,
	wt::WtVec3 &v,
	wt::WtVec3 &w
) {
	switch (orientation) {
		case wt::WtTransitionOrientation::PositiveX: u = { 0, 1, 0 }; v = { 0, 0, 1 }; w = { 1, 0, 0 }; break;
		case wt::WtTransitionOrientation::NegativeX: u = { 0, 1, 0 }; v = { 0, 0, -1 }; w = { -1, 0, 0 }; break;
		case wt::WtTransitionOrientation::PositiveY: u = { 0, 0, 1 }; v = { 1, 0, 0 }; w = { 0, 1, 0 }; break;
		case wt::WtTransitionOrientation::NegativeY: u = { 0, 0, 1 }; v = { -1, 0, 0 }; w = { 0, -1, 0 }; break;
		case wt::WtTransitionOrientation::PositiveZ: u = { 1, 0, 0 }; v = { 0, 1, 0 }; w = { 0, 0, 1 }; break;
		case wt::WtTransitionOrientation::NegativeZ: u = { 1, 0, 0 }; v = { 0, -1, 0 }; w = { 0, 0, -1 }; break;
	}
}

std::uint64_t test_regular_cases(const wt::WtMeshingBackend &backend) {
	std::uint64_t hash = 14695981039346656037ULL;
	wt::WtCellMeshingScratch scratch;
	for (unsigned int case_code = 0; case_code < 256; ++case_code) {
		wt::WtRegularCellInput input = make_regular_input(case_code);
		wt::WtCellMesh mesh;
		const wt::WtCellStatus status = backend.mesh_regular_cell(input, mesh, scratch);
		const bool expected_empty = case_code == 0 || case_code == 255;
		check((status == wt::WtCellStatus::Empty) == expected_empty, "regular empty classification mismatch");
		if (status == wt::WtCellStatus::Ok) {
			validate_mesh(mesh, input.samples.data(), input.samples.size(), 8);
			for (std::uint8_t index = 0; index < mesh.vertex_count; ++index) {
				const wt::WtVec3 p = mesh.vertices[index].position;
				check(p.x >= 0.0F && p.x <= 1.0F && p.y >= 0.0F && p.y <= 1.0F &&
					p.z >= 0.0F && p.z <= 1.0F, "regular vertex outside cell");
			}
		}
		wt::WtCellMesh repeated;
		wt::WtCellMeshingScratch repeated_scratch;
		const wt::WtCellStatus repeated_status = backend.mesh_regular_cell(input, repeated, repeated_scratch);
		check(status == repeated_status && equal_mesh(mesh, repeated), "regular result is not deterministic");
		hash_u16(hash, static_cast<std::uint16_t>(case_code));
		hash_mesh(hash, status, mesh);
	}
	return hash;
}

std::uint64_t test_transition_cases(const wt::WtMeshingBackend &backend) {
	std::uint64_t hash = 14695981039346656037ULL;
	for (unsigned int case_code = 0; case_code < 512; ++case_code) {
		wt::WtTransitionCellInput canonical_input = make_transition_input(case_code);
		canonical_input.orientation = wt::WtTransitionOrientation::PositiveZ;
		wt::WtCellMesh canonical_mesh;
		wt::WtCellMeshingScratch canonical_scratch;
		const wt::WtCellStatus canonical_status = backend.mesh_transition_cell(
			canonical_input,
			canonical_mesh,
			canonical_scratch
		);
		const bool expected_empty = case_code == 0 || case_code == 511;
		check((canonical_status == wt::WtCellStatus::Empty) == expected_empty,
			"transition empty classification mismatch");
		for (unsigned int orientation_index = 0; orientation_index < 6; ++orientation_index) {
			wt::WtTransitionCellInput input = make_transition_input(case_code);
			input.orientation = static_cast<wt::WtTransitionOrientation>(orientation_index);
			wt::WtCellMesh mesh;
			wt::WtCellMeshingScratch scratch;
			const wt::WtCellStatus status = backend.mesh_transition_cell(input, mesh, scratch);
			check(status == canonical_status, "transition orientation changed status");
			if (status == wt::WtCellStatus::Ok) {
				validate_mesh(mesh, input.samples.data(), input.samples.size(), 13);
				check(mesh.vertex_count == canonical_mesh.vertex_count &&
					mesh.index_count == canonical_mesh.index_count, "transition orientation changed counts");
				wt::WtVec3 u;
				wt::WtVec3 v;
				wt::WtVec3 w;
				get_basis(input.orientation, u, v, w);
				check(equal_vec(cross(u, v), w), "transition orientation is not right-handed");
				for (std::uint8_t index = 0; index < mesh.vertex_count; ++index) {
					const wt::WtVec3 p = subtract(mesh.vertices[index].position, input.full_resolution_origin);
					const wt::WtVec3 local = { dot(p, u), dot(p, v), dot(p, w) };
					check(equal_vec(local, canonical_mesh.vertices[index].position),
						"transition orientation changed canonical vertex");
					check(local.x >= 0.0F && local.x <= 2.0F && local.y >= 0.0F &&
						local.y <= 2.0F && local.z >= 0.0F && local.z <= 0.25F,
						"transition vertex outside cell prism");
				}
				for (std::uint8_t index = 0; index < mesh.index_count; ++index) {
					check(mesh.indices[index] == canonical_mesh.indices[index],
						"transition orientation changed winding/index order");
				}
			}
			hash_u16(hash, static_cast<std::uint16_t>(case_code));
			hash_byte(hash, static_cast<std::uint8_t>(input.orientation));
			hash_mesh(hash, status, mesh);
		}
	}
	return hash;
}

void test_contract_edges(const wt::WtMeshingBackend &backend) {
	wt::WtCellMeshingScratch scratch;
	wt::WtCellMesh mesh;

	wt::WtRegularCellInput regular = make_regular_input(1);
	check(backend.mesh_regular_cell(regular, mesh, scratch) == wt::WtCellStatus::Ok,
		"regular anchor case failed");
	check(mesh.vertex_count == 3 && mesh.index_count == 3, "regular anchor topology mismatch");
	const wt::WtVec3 regular_normal = cross(
		subtract(mesh.vertices[mesh.indices[1]].position, mesh.vertices[mesh.indices[0]].position),
		subtract(mesh.vertices[mesh.indices[2]].position, mesh.vertices[mesh.indices[0]].position)
	);
	check(dot(regular_normal, { 1.0F, 1.0F, 1.0F }) > 0.0F, "regular winding is not outward");
	regular.samples[0].gradient = { 1.0F, 0.0F, 0.0F };
	regular.samples[1].density = 3.0F;
	regular.samples[1].gradient = { 0.0F, 1.0F, 0.0F };
	check(backend.mesh_regular_cell(regular, mesh, scratch) == wt::WtCellStatus::Ok,
		"regular interpolation probe failed");
	check(equal_vec(mesh.vertices[0].position, { 0.25F, 0.0F, 0.0F }),
		"regular interpolation position mismatch");
	const float inverse_probe_length = 1.0F / std::sqrt(0.625F);
	check(equal_vec(mesh.vertices[0].normal,
		{ 0.75F * inverse_probe_length, 0.25F * inverse_probe_length, 0.0F }),
		"regular gradient interpolation mismatch");
	check(mesh.vertices[0].material == 1, "regular solid-endpoint material mismatch");

	wt::WtTransitionCellInput transition = make_transition_input(1);
	check(backend.mesh_transition_cell(transition, mesh, scratch) == wt::WtCellStatus::Ok,
		"transition anchor case failed");
	check(mesh.vertex_count == 4 && mesh.index_count == 6, "transition anchor topology mismatch");
	const wt::WtVec3 transition_normal = cross(
		subtract(mesh.vertices[mesh.indices[1]].position, mesh.vertices[mesh.indices[0]].position),
		subtract(mesh.vertices[mesh.indices[2]].position, mesh.vertices[mesh.indices[0]].position)
	);
	check(dot(transition_normal, { 1.0F, 1.0F, 0.0F }) > 0.0F,
		"transition representative winding mismatch");
	check(nearly_equal(mesh.vertices[2].position.z, 0.25F) &&
		nearly_equal(mesh.vertices[3].position.z, 0.25F),
		"transition half-resolution face width was flattened");

	transition = make_transition_input(510);
	check(backend.mesh_transition_cell(transition, mesh, scratch) == wt::WtCellStatus::Ok,
		"transition inverse anchor case failed");
	wt::WtVec3 inverse_normal_sum;
	for (std::uint8_t index = 0; index < mesh.index_count; index += 3) {
		const wt::WtVec3 triangle_normal = cross(
			subtract(mesh.vertices[mesh.indices[index + 1]].position,
				mesh.vertices[mesh.indices[index]].position),
			subtract(mesh.vertices[mesh.indices[index + 2]].position,
				mesh.vertices[mesh.indices[index]].position)
		);
		inverse_normal_sum.x += triangle_normal.x;
		inverse_normal_sum.y += triangle_normal.y;
		inverse_normal_sum.z += triangle_normal.z;
	}
	check(dot(inverse_normal_sum, { -1.0F, -1.0F, 0.0F }) > 0.0F,
		"transition inverse winding mismatch");

	regular = make_regular_input(0);
	regular.samples[0].density = 0.0F;
	for (unsigned int index = 1; index < regular.samples.size(); ++index) {
		regular.samples[index].density = -1.0F;
	}
	check(backend.mesh_regular_cell(regular, mesh, scratch) == wt::WtCellStatus::Empty,
		"exact-isovalue degenerate triangle was not removed");

	regular = make_regular_input(1);
	for (wt::WtCellSample &sample : regular.samples) {
		sample.density += 7.0F;
	}
	regular.isovalue = 7.0F;
	wt::WtCellMesh shifted_mesh;
	check(backend.mesh_regular_cell(regular, shifted_mesh, scratch) == wt::WtCellStatus::Ok,
		"shifted isovalue failed");
	check(shifted_mesh.vertex_count == 3 && shifted_mesh.index_count == 3,
		"shifted isovalue changed topology");

	regular.cell_size = 0.0F;
	check(backend.mesh_regular_cell(regular, mesh, scratch) == wt::WtCellStatus::InvalidScale,
		"zero regular scale was accepted");
	regular.cell_size = 1.0F;
	regular.samples[0].density = std::numeric_limits<float>::quiet_NaN();
	check(backend.mesh_regular_cell(regular, mesh, scratch) == wt::WtCellStatus::NonFiniteInput,
		"NaN density was accepted");

	transition = make_transition_input(1);
	transition.transition_width = 0.0F;
	check(backend.mesh_transition_cell(transition, mesh, scratch) == wt::WtCellStatus::InvalidScale,
		"zero transition width was accepted");
	transition.transition_width = 0.25F;
	transition.orientation = static_cast<wt::WtTransitionOrientation>(99);
	check(backend.mesh_transition_cell(transition, mesh, scratch) == wt::WtCellStatus::InvalidOrientation,
		"invalid transition orientation was accepted");

	check(&wt::wt_get_thread_cell_meshing_scratch() == &wt::wt_get_thread_cell_meshing_scratch(),
		"thread-local scratch was not reused");
}

} // namespace

int main() {
	const wt::WtMeshingBackend &backend = wt::wt_get_transvoxel_mit_backend();
	check(backend.is_available(), "MIT backend unavailable");
	check(backend.get_info().regular_case_count == 256, "regular metadata count mismatch");
	check(backend.get_info().transition_case_count == 512, "transition metadata count mismatch");
	check(wt::kWtDefaultChunkCellsPerAxis == 16 && wt::kWtChunkMeshingSamplesPerAxis == 19,
		"locked chunk/sample dimensions changed");

	test_contract_edges(backend);
	const std::uint64_t regular_hash = test_regular_cases(backend);
	const std::uint64_t transition_hash = test_transition_cases(backend);

	constexpr std::uint64_t kExpectedRegularHash = 0x21294cb1188f4c45ULL;
	constexpr std::uint64_t kExpectedTransitionHash = 0x94e9e05bd1719331ULL;
	check(regular_hash == kExpectedRegularHash, "regular aggregate hash mismatch");
	check(transition_hash == kExpectedTransitionHash, "transition aggregate hash mismatch");

	std::printf(
		"M1_HASHES regular=%016llx transition=%016llx\n",
		static_cast<unsigned long long>(regular_hash),
		static_cast<unsigned long long>(transition_hash)
	);
	if (failure_count != 0) {
		std::fprintf(stderr, "M1_CELL_BACKEND_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M1_CELL_BACKEND_PASS regular_cases=256 transition_orientations=3072\n");
	return 0;
}
