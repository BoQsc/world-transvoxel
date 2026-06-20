#pragma once

#include "meshing/wt_chunk_mesher.h"

#include <cstdint>
#include <map>
#include <set>

namespace world_transvoxel_test {

namespace wt = world_transvoxel;

extern int failure_count;

void check(bool condition, const char *message);

struct LinearSource final : wt::WtChunkSampleSource {
	double threshold = 0.0;

	bool sample(const wt::WtGridPoint &point, wt::WtScalarSample &output) const noexcept override;
};

struct SphereSource final : wt::WtChunkSampleSource {
	wt::WtGridPoint center;
	double radius = 8.25;

	bool sample(const wt::WtGridPoint &point, wt::WtScalarSample &output) const noexcept override;
};

struct FailingSource final : wt::WtChunkSampleSource {
	bool sample(const wt::WtGridPoint &, wt::WtScalarSample &) const noexcept override;
};

struct InvalidBackend final : wt::WtMeshingBackend {
	const wt::WtMeshingBackendInfo &get_info() const noexcept override;
	bool is_available() const noexcept override;
	wt::WtCellStatus mesh_regular_cell(
		const wt::WtRegularCellInput &input,
		wt::WtCellMesh &output,
		wt::WtCellMeshingScratch &scratch
	) const noexcept override;
	wt::WtCellStatus mesh_transition_cell(
		const wt::WtTransitionCellInput &input,
		wt::WtCellMesh &output,
		wt::WtCellMeshingScratch &scratch
	) const noexcept override;
};

struct QuantizedPoint {
	std::int64_t x;
	std::int64_t y;
	std::int64_t z;

	bool operator<(const QuantizedPoint &other) const noexcept;
	bool operator==(const QuantizedPoint &other) const noexcept;
};

struct Edge {
	QuantizedPoint a;
	QuantizedPoint b;

	bool operator<(const Edge &other) const noexcept;
	bool operator==(const Edge &other) const noexcept;
};

using EdgeCounts = std::map<Edge, unsigned int>;
using EdgeSet = std::set<Edge>;

EdgeSet plane_boundary_edges(
	const wt::WtChunkMeshBuffer &mesh,
	const wt::WtGridPoint &origin,
	int axis,
	double plane
);

void add_mesh_edges(
	const wt::WtChunkMeshBuffer &mesh,
	const wt::WtGridPoint &origin,
	EdgeCounts &counts,
	const wt::WtGridPoint &reference = {}
);

void add_result_edges(const wt::WtChunkMeshResult &result, EdgeCounts &counts);
void check_closed_surface(const EdgeCounts &counts, const char *message);
void validate_buffer(const wt::WtChunkMeshBuffer &mesh, const char *message);
void hash_result(std::uint64_t &hash, const wt::WtChunkMeshResult &result);

} // namespace world_transvoxel_test
