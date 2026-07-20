#include "backend/wt_transvoxel_mit_backend.h"
#include "bake/wt_chunk_baker.h"
#include "editing/wt_chunk_edit_state.h"
#include "meshing/wt_material_volume_sample_source.h"
#include "render/wt_render_payload.h"
#include "services/wt_page_meshing_runtime.h"
#include "storage/wt_async_storage_service.h"
#include "storage/wt_chunk_page_sample_source.h"
#include "storage/wt_hash256.h"
#include "storage/wt_procedural_world_source.h"
#include "storage/wt_storage_page_cache.h"
#include "wt_m2_mesh_test_support.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wt = world_transvoxel;
using namespace world_transvoxel_test;

namespace {

constexpr std::uint64_t kSourceRevision = 7101;
constexpr std::uint64_t kWorldRevision = 91;
constexpr std::size_t kDependencyCount = 13;

wt::WtHash256 hash_text(const char *text) {
	const std::string value(text);
	return wt::wt_sha256(
		reinterpret_cast<const std::uint8_t *>(value.data()),
		value.size()
	);
}

wt::WtDependencyEntry dependency(
	wt::WtDependencyKind kind,
	const char *label,
	const char *version,
	const char *identity
) {
	return { kind, label, version, hash_text(identity) };
}

class SphereSource final : public wt::WtChunkSampleSource {
public:
	bool sample(
		const wt::WtGridPoint &point,
		wt::WtScalarSample &output
	) const noexcept override {
		const double dx = static_cast<double>(point.x) - 32.0;
		const double dy = static_cast<double>(point.y) - 32.0;
		const double dz = static_cast<double>(point.z) - 32.0;
		output.density = static_cast<float>(
			std::sqrt(dx * dx + dy * dy + dz * dz) - 8.25
		);
		output.material = 7;
		return true;
	}
};

class SphereDifferenceSource final : public wt::WtChunkSampleSource {
public:
	SphereDifferenceSource(
		double outer_radius,
		double cavity_offset,
		double cavity_radius,
		double smooth_radius = 0.0
	) noexcept : outer_radius_(outer_radius),
		cavity_offset_(cavity_offset), cavity_radius_(cavity_radius),
		smooth_radius_(smooth_radius) {
	}

	bool sample(
		const wt::WtGridPoint &point,
		wt::WtScalarSample &output
	) const noexcept override {
		constexpr double center = 32.0;
		constexpr double boundary_epsilon = 0.01;
		const double x = static_cast<double>(point.x) - center;
		const double y = static_cast<double>(point.y) - center;
		const double z = static_cast<double>(point.z) - center;
		double construct_brush = outer_radius_ - std::sqrt(x * x + y * y + z * z);
		if (std::abs(construct_brush) < boundary_epsilon) {
			construct_brush = construct_brush < 0.0 ?
				-boundary_epsilon : boundary_epsilon;
		}
		const double cavity_y = y - cavity_offset_;
		double carve_brush = cavity_radius_ - std::sqrt(
			x * x + cavity_y * cavity_y + z * z
		);
		if (std::abs(carve_brush) < boundary_epsilon) {
			carve_brush = carve_brush < 0.0 ?
				-boundary_epsilon : boundary_epsilon;
		}
		const double constructed_density = -construct_brush;
		double density = std::max(constructed_density, carve_brush);
		if (smooth_radius_ > 0.0) {
			const double h = std::clamp(
				0.5 + 0.5 * (constructed_density - carve_brush) /
					smooth_radius_,
				0.0,
				1.0
			);
			density = carve_brush +
				(constructed_density - carve_brush) * h +
				smooth_radius_ * h * (1.0 - h);
		}
		output.density = static_cast<float>(density);
		output.material = 1;
		return true;
	}

private:
	double outer_radius_ = 0.0;
	double cavity_offset_ = 0.0;
	double cavity_radius_ = 0.0;
	double smooth_radius_ = 0.0;
};

class MaterialVolumeDistanceSource final : public wt::WtChunkSampleSource {
public:
	explicit MaterialVolumeDistanceSource(std::int64_t spacing = 1) noexcept :
		spacing_(spacing) {
	}

	bool sample(
		const wt::WtGridPoint &point,
		wt::WtScalarSample &output
	) const noexcept override {
		if (spacing_ <= 0 || point.x % spacing_ != 0 ||
			point.y % spacing_ != 0 || point.z % spacing_ != 0) {
			return false;
		}
		if (point.x == 0 && point.y <= 0) {
			output = { 3.0F, wt::kWtStaticWaterMaterialId };
		} else if (point.x == -1) {
			output = { -0.25F, 1 };
		} else if (point.x == 10) {
			output = { 0.75F, 0 };
		} else {
			output = { 0.5F, 0 };
		}
		return true;
	}

private:
	std::int64_t spacing_ = 1;
};

class FourBiomeProceduralSource final : public wt::WtChunkSampleSource {
public:
	FourBiomeProceduralSource() noexcept {
		descriptor_.chunk_count_x = 128;
		descriptor_.chunk_count_y = 16;
		descriptor_.chunk_count_z = 128;
		descriptor_.chunk_y = -8;
		descriptor_.source_revision = 190325;
		descriptor_.seed = 19023;
		descriptor_.mode = wt::WtProceduralWorldMode::FourBiomesLakesCavesRoads;
	}

	bool sample(
		const wt::WtGridPoint &point,
		wt::WtScalarSample &output
	) const noexcept override {
		return wt::wt_sample_procedural_world(descriptor_, point, output);
	}

private:
	wt::WtProceduralWorldDescriptor descriptor_;
};

struct ReproSphereEdit {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double radius = 0.0;
};

std::int64_t q16(double value) {
	return static_cast<std::int64_t>(std::llround(
		value * static_cast<double>(wt::kWtEditCoordinateScale)
	));
}

std::uint64_t uq16(double value) {
	return static_cast<std::uint64_t>(std::llround(
		value * static_cast<double>(wt::kWtEditCoordinateScale)
	));
}

wt::WtId128 repro_id(std::uint64_t seed) {
	wt::WtId128 value{};
	for (std::size_t index = 0; index < value.size(); ++index) {
		value[index] = static_cast<std::uint8_t>(
			(seed + 1U) * 31U + index * 13U
		);
	}
	return value;
}

double mesh_triangle_normal_alignment(
	const wt::WtChunkMeshBuffer &mesh,
	std::size_t index
) {
	const std::uint32_t triangle[3] = {
		mesh.indices[index], mesh.indices[index + 1], mesh.indices[index + 2]
	};
	const wt::WtCellVertex &vertex_a = mesh.vertices[triangle[0]];
	const wt::WtCellVertex &vertex_b = mesh.vertices[triangle[1]];
	const wt::WtCellVertex &vertex_c = mesh.vertices[triangle[2]];
	const double ab_x =
		static_cast<double>(vertex_b.position.x) - vertex_a.position.x;
	const double ab_y =
		static_cast<double>(vertex_b.position.y) - vertex_a.position.y;
	const double ab_z =
		static_cast<double>(vertex_b.position.z) - vertex_a.position.z;
	const double ac_x =
		static_cast<double>(vertex_c.position.x) - vertex_a.position.x;
	const double ac_y =
		static_cast<double>(vertex_c.position.y) - vertex_a.position.y;
	const double ac_z =
		static_cast<double>(vertex_c.position.z) - vertex_a.position.z;
	const double cross_x = ab_y * ac_z - ab_z * ac_y;
	const double cross_y = ab_z * ac_x - ab_x * ac_z;
	const double cross_z = ab_x * ac_y - ab_y * ac_x;
	const double normal_x =
		static_cast<double>(vertex_a.normal.x) +
		static_cast<double>(vertex_b.normal.x) +
		static_cast<double>(vertex_c.normal.x);
	const double normal_y =
		static_cast<double>(vertex_a.normal.y) +
		static_cast<double>(vertex_b.normal.y) +
		static_cast<double>(vertex_c.normal.y);
	const double normal_z =
		static_cast<double>(vertex_a.normal.z) +
		static_cast<double>(vertex_b.normal.z) +
		static_cast<double>(vertex_c.normal.z);
	return cross_x * normal_x + cross_y * normal_y + cross_z * normal_z;
}

int face_axis(wt::WtChunkFace face) {
	return static_cast<int>(face) / 2;
}

bool positive_face(wt::WtChunkFace face) {
	return (static_cast<unsigned int>(face) & 1U) != 0U;
}

QuantizedPoint quantize_test_point(
	const wt::WtVec3 &position,
	const wt::WtGridPoint &origin
) {
	constexpr double scale = 1000000.0;
	return {
		static_cast<std::int64_t>(std::llround(
			(static_cast<double>(origin.x) + position.x) * scale)),
		static_cast<std::int64_t>(std::llround(
			(static_cast<double>(origin.y) + position.y) * scale)),
		static_cast<std::int64_t>(std::llround(
			(static_cast<double>(origin.z) + position.z) * scale)),
	};
}

using PointAdjacency = std::map<QuantizedPoint, std::set<QuantizedPoint>>;

void append_mesh_connectivity(
	const wt::WtChunkMeshBuffer &mesh,
	const wt::WtGridPoint &origin,
	PointAdjacency &adjacency
) {
	for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
		const QuantizedPoint points[3] = {
			quantize_test_point(mesh.vertices[mesh.indices[index]].position, origin),
			quantize_test_point(mesh.vertices[mesh.indices[index + 1]].position, origin),
			quantize_test_point(mesh.vertices[mesh.indices[index + 2]].position, origin),
		};
		for (unsigned int vertex = 0; vertex < 3; ++vertex) {
			const QuantizedPoint &a = points[vertex];
			const QuantizedPoint &b = points[(vertex + 1U) % 3U];
			adjacency[a].insert(b);
			adjacency[b].insert(a);
		}
	}
}

std::size_t connected_component_count(const PointAdjacency &adjacency) {
	std::set<QuantizedPoint> visited;
	std::size_t components = 0;
	for (const auto &entry : adjacency) {
		if (visited.find(entry.first) != visited.end()) {
			continue;
		}
		++components;
		std::vector<QuantizedPoint> pending = { entry.first };
		visited.insert(entry.first);
		while (!pending.empty()) {
			const QuantizedPoint point = pending.back();
			pending.pop_back();
			const auto found = adjacency.find(point);
			if (found == adjacency.end()) {
				continue;
			}
			for (const QuantizedPoint &neighbor : found->second) {
				if (visited.insert(neighbor).second) {
					pending.push_back(neighbor);
				}
			}
		}
	}
	return components;
}

std::size_t sphere_difference_component_count(
	const SphereDifferenceSource &source,
	std::uint8_t lod
) {
	const wt::WtChunkMesher mesher(wt::wt_get_transvoxel_mit_backend());
	wt::WtChunkMeshingScratch scratch;
	PointAdjacency adjacency;
	const std::int64_t extent = wt::wt_chunk_extent(lod);
	const std::int32_t minimum_key = static_cast<std::int32_t>(3 / extent);
	const std::int32_t maximum_key = static_cast<std::int32_t>(61 / extent);
	for (std::int32_t z = minimum_key; z <= maximum_key; ++z) {
		for (std::int32_t y = minimum_key; y <= maximum_key; ++y) {
			for (std::int32_t x = minimum_key; x <= maximum_key; ++x) {
				wt::WtChunkMeshResult result;
				const wt::WtChunkKey key = { x, y, z, lod };
				check(mesher.mesh(
						{ key, 0, 0.0F, 0.25F }, source, result, scratch
					) == wt::WtChunkMeshingStatus::Ok,
					"sphere difference topology mesh failed");
				append_mesh_connectivity(result.regular, result.world_origin, adjacency);
			}
		}
	}
	return connected_component_count(adjacency);
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value);

void test_representable_sphere_difference_topology(
	std::vector<std::uint8_t> &evidence
) {
	const SphereDifferenceSource near_tangent(24.0, 11.0, 14.0);
	const std::size_t near_tangent_components =
		sphere_difference_component_count(near_tangent, 0);
	check(near_tangent_components > 1,
		"sphere difference topology control did not reproduce detached components");
	const SphereDifferenceSource representable(28.0, 19.0, 22.0);
	const SphereDifferenceSource smooth_representable(28.0, 19.0, 22.0, 3.0);
	for (std::uint8_t lod = 0; lod <= 2; ++lod) {
		const std::size_t components =
			sphere_difference_component_count(representable, lod);
		check(components == 1,
			"representable sphere difference produced detached components");
		append_u64(evidence, components);
		const std::size_t smooth_components =
			sphere_difference_component_count(smooth_representable, lod);
		check(smooth_components == 1,
			"smooth sphere difference produced detached components");
		append_u64(evidence, smooth_components);
	}
	append_u64(evidence, near_tangent_components);
}

void test_material_volume_continuous_distance(
	std::vector<std::uint8_t> &evidence
) {
	const MaterialVolumeDistanceSource terrain;
	const wt::WtMaterialVolumeSampleSource water(
		terrain, wt::kWtStaticWaterMaterialId
	);
	wt::WtScalarSample sample;
	check(water.sample({ 0, 0, 0 }, sample) && sample.density == -0.5F,
		"material volume did not preserve the free-surface distance below water");
	check(water.sample({ 0, -3, 0 }, sample) && sample.density == -3.5F,
		"material volume lost continuous depth below the free surface");
	check(water.sample({ -1, 0, 0 }, sample) && sample.density == -0.25F,
		"material volume lost continuous solid terrain distance");
	check(water.sample({ 0, 1, 0 }, sample) && sample.density == 0.5F,
		"material volume lost continuous air distance above water");
	check(water.sample({ 10, 1, 0 }, sample) && sample.density == -0.75F,
		"material volume did not suppress unrelated air");
	for (const std::int64_t spacing : { 2, 4, 8 }) {
		const MaterialVolumeDistanceSource spaced_terrain(spacing);
		const wt::WtMaterialVolumeSampleSource spaced_water(
			spaced_terrain, wt::kWtStaticWaterMaterialId
		);
		check(spaced_water.sample({ 0, 0, 0 }, sample) &&
			sample.density == -0.5F * static_cast<float>(spacing),
			"material volume lost the free surface below a coarse page sample");
		check(spaced_water.sample({ 0, spacing, 0 }, sample) &&
			sample.density == 0.5F * static_cast<float>(spacing),
			"material volume lost the free surface above a coarse page sample");
	}
	append_u64(evidence, 1);
}

void test_four_biome_water_free_surface(
	std::vector<std::uint8_t> &evidence
) {
	const FourBiomeProceduralSource terrain;
	const wt::WtMaterialVolumeSampleSource water(
		terrain, wt::kWtStaticWaterMaterialId
	);
	const wt::WtChunkMesher mesher(wt::wt_get_transvoxel_mit_backend());
	wt::WtChunkMeshingScratch terrain_scratch;
	wt::WtChunkMeshingScratch water_scratch;
	for (std::uint8_t lod = 0; lod <= 3; ++lod) {
		const std::int64_t extent = wt::wt_chunk_extent(lod);
		const wt::WtChunkKey key = {
			static_cast<std::int32_t>(650 / extent),
			static_cast<std::int32_t>(23 / extent),
			static_cast<std::int32_t>(700 / extent),
			lod,
		};
		wt::WtChunkMeshResult terrain_mesh;
		wt::WtChunkMeshResult water_mesh;
		check(mesher.mesh(
				{ key, 0, 0.0F, 0.25F }, terrain, terrain_mesh, terrain_scratch
			) == wt::WtChunkMeshingStatus::Ok,
			"four-biome lake terrain mesh failed");
		check(mesher.mesh(
				{ key, 0, 0.0F, 0.25F }, water, water_mesh, water_scratch
			) == wt::WtChunkMeshingStatus::Ok,
			"four-biome lake water mesh failed");
		wt::WtRenderPayload render;
		check(wt::wt_build_render_payload(
				terrain_mesh, water_mesh, { 1 }, render
			) == wt::WtRenderBuildStatus::Ok,
			"four-biome lake render payload failed");
		check(!render.water_indices.empty(),
			"four-biome lake free surface was filtered out");
		const float expected_local_level = 23.5F -
			static_cast<float>(wt::wt_chunk_bounds(key).minimum.y);
		for (const std::uint32_t index : render.water_indices) {
			check(std::abs(
					render.water_vertices[index].position.y -
					expected_local_level
				) <= 0.011F,
				"four-biome lake free surface moved between LODs");
		}
		append_u64(evidence, render.water_indices.size());
	}
	std::array<double, 4> shoreline_area{};
	for (std::uint8_t lod = 0; lod <= 3; ++lod) {
		const std::int64_t extent = wt::wt_chunk_extent(lod);
		const std::int32_t chunk_min_x = static_cast<std::int32_t>(768 / extent);
		const std::int32_t chunk_min_z = static_cast<std::int32_t>(640 / extent);
		const std::int32_t chunk_count = static_cast<std::int32_t>(128 / extent);
		for (std::int32_t z = 0; z < chunk_count; ++z) {
			for (std::int32_t x = 0; x < chunk_count; ++x) {
				const wt::WtChunkKey key = {
					chunk_min_x + x,
					static_cast<std::int32_t>(23 / extent),
					chunk_min_z + z,
					lod,
				};
				wt::WtChunkMeshResult terrain_mesh;
				wt::WtChunkMeshResult water_mesh;
				check(mesher.mesh(
						{ key, 0, 0.0F, 0.25F }, terrain,
						terrain_mesh, terrain_scratch
					) == wt::WtChunkMeshingStatus::Ok,
					"four-biome shoreline terrain mesh failed");
				check(mesher.mesh(
						{ key, 0, 0.0F, 0.25F }, water,
						water_mesh, water_scratch
					) == wt::WtChunkMeshingStatus::Ok,
					"four-biome shoreline water mesh failed");
				wt::WtRenderPayload render;
				check(wt::wt_build_render_payload(
						terrain_mesh, water_mesh, { 1 }, render
					) == wt::WtRenderBuildStatus::Ok,
					"four-biome shoreline render payload failed");
				for (std::size_t triangle = 0;
					triangle < render.water_indices.size(); triangle += 3U) {
					const wt::WtVec3 &a = render.water_vertices[
						render.water_indices[triangle]
					].position;
					const wt::WtVec3 &b = render.water_vertices[
						render.water_indices[triangle + 1U]
					].position;
					const wt::WtVec3 &c = render.water_vertices[
						render.water_indices[triangle + 2U]
					].position;
					shoreline_area[lod] += 0.5 * std::abs(
						static_cast<double>(b.x - a.x) * (c.z - a.z) -
						static_cast<double>(b.z - a.z) * (c.x - a.x)
					);
				}
			}
		}
	}
	std::printf(
		"M5_WATER_SHORELINE_AREA %.3f %.3f %.3f %.3f\n",
		shoreline_area[0], shoreline_area[1],
		shoreline_area[2], shoreline_area[3]
	);
	const auto footprint_range = std::minmax_element(
		shoreline_area.begin(), shoreline_area.end()
	);
	check(
		*footprint_range.first > 0.0 &&
		(*footprint_range.second - *footprint_range.first) <=
			*footprint_range.second * 0.002,
		"four-biome lake shoreline footprint moved between LODs"
	);
}

Edge make_test_edge(QuantizedPoint a, QuantizedPoint b) {
	if (b < a) {
		const QuantizedPoint temporary = a;
		a = b;
		b = temporary;
	}
	return { a, b };
}

std::vector<wt::WtEditCommand> human_boundary_repro_commands() {
	const ReproSphereEdit edits[] = {
		{ 1183.96289, 119.422333, 1006.94098, 1.80000305 },
		{ 1183.94055, 117.829788, 1006.30176, 1.80000305 },
		{ 1183.92468, 116.166275, 1005.85016, 1.80000305 },
		{ 1183.90125, 114.547058, 1005.34998, 1.80000305 },
		{ 1183.89453, 112.876434, 1004.86182, 1.80000305 },
		{ 1183.92114, 111.186813, 1004.65564, 1.80000305 },
		{ 1183.85681, 110.805527, 1002.97369, 1.80000305 },
		{ 1183.80139, 109.935593, 1001.49847, 1.80000305 },
		{ 1183.68286, 109.570236, 999.88208, 1.80000305 },
		{ 1183.61780, 108.692322, 998.513977, 1.80000305 },
		{ 1183.55176, 107.788940, 997.123230, 1.80000305 },
		{ 1183.48438, 106.879608, 995.706116, 1.80000305 },
	};
	std::vector<wt::WtEditCommand> commands;
	commands.reserve(std::size(edits));
	for (std::size_t index = 0; index < std::size(edits); ++index) {
		wt::WtEditCommand command;
		command.command_id = repro_id(index);
		command.sequence = 0;
		command.world_revision = static_cast<std::uint64_t>(index + 1U);
		command.operation = wt::WtEditOperation::SdfCarve;
		command.shape = wt::WtEditShape::Sphere;
		command.density_value = 1.0F;
		command.sphere = {
			q16(edits[index].x),
			q16(edits[index].y),
			q16(edits[index].z),
			uq16(edits[index].radius),
		};
		check(wt::wt_edit_sphere_bounds(command.sphere, command.bounds),
			"human boundary repro edit bounds failed");
		commands.push_back(command);
	}
	return commands;
}

wt::WtProceduralWorldDescriptor human_boundary_repro_descriptor() {
	wt::WtProceduralWorldDescriptor descriptor;
	descriptor.chunk_count_x = 128;
	descriptor.chunk_count_z = 128;
	descriptor.chunk_y = 0;
	descriptor.seed = 19019;
	descriptor.source_revision = 190019;
	descriptor.mode = wt::WtProceduralWorldMode::Terrain;
	return descriptor;
}

bool try_generate_procedural_page(
	const wt::WtProceduralWorldDescriptor &descriptor,
	const wt::WtChunkKey &key,
	wt::WtChunkPage &page
) {
	std::uint64_t bytes_read = 0;
	const wt::WtPageLoadCompletion completion =
		wt::wt_generate_procedural_page(descriptor, key, {}, bytes_read);
	if (completion.status != wt::WtPageLoadStatus::Ok ||
		completion.page_bytes == nullptr) {
		return false;
	}
	wt::WtChunkPageView view;
	if (wt::wt_open_chunk_page(
			{ completion.page_bytes->data(), completion.page_bytes->size() },
			view
		) != wt::WtChunkPageStatus::Ok) {
		return false;
	}
	if (wt::wt_decode_chunk_page(view, page) != wt::WtChunkPageStatus::Ok) {
		return false;
	}
	if (page.metadata.key != key) {
		return false;
	}
	return true;
}

wt::WtChunkPage generate_procedural_page(
	const wt::WtProceduralWorldDescriptor &descriptor,
	const wt::WtChunkKey &key
) {
	wt::WtChunkPage page;
	check(try_generate_procedural_page(descriptor, key, page),
		"human boundary repro page generation failed");
	return page;
}

wt::WtChunkPage edited_procedural_page(
	const wt::WtProceduralWorldDescriptor &descriptor,
	const wt::WtChunkKey &key,
	const std::vector<wt::WtEditCommand> &commands
) {
	wt::WtChunkEditState edit_state;
	check(edit_state.initialize(
			generate_procedural_page(descriptor, key),
			descriptor.source_revision,
			0
		) == wt::WtChunkEditStatus::Ok,
		"human boundary repro edit state init failed");
	for (const wt::WtEditCommand &command : commands) {
		check(edit_state.apply_command(command) == wt::WtChunkEditStatus::Ok,
			"human boundary repro edit apply failed");
	}
	return edit_state.page();
}

wt::WtChunkMeshResult mesh_page_direct(
	const wt::WtChunkMesher &mesher,
	wt::WtChunkMeshingScratch &scratch,
	const wt::WtChunkPage &page
) {
	wt::WtChunkPageSampleSource source(page);
	check(source.status() == wt::WtChunkPageSampleSourceStatus::Ok,
		"human boundary repro page sample source failed");
	wt::WtChunkMeshResult result;
	check(mesher.mesh(
			{ page.metadata.key, 0, 0.0F, 0.25F },
			source,
			result,
			scratch
		) == wt::WtChunkMeshingStatus::Ok,
		"human boundary repro mesh failed");
	validate_buffer(result.regular, "invalid human boundary repro regular mesh");
	return result;
}

bool try_mesh_procedural_page_with_transition_support(
	const wt::WtChunkMesher &mesher,
	wt::WtChunkMeshingScratch &scratch,
	const wt::WtProceduralWorldDescriptor &descriptor,
	const wt::WtChunkKey &key,
	std::uint8_t transition_mask,
	wt::WtChunkMeshResult &result
) {
	std::vector<wt::WtChunkPage> pages;
	pages.reserve(1U + wt::kWtMaximumTransitionSupportPages);
	pages.emplace_back();
	if (!try_generate_procedural_page(descriptor, key, pages.back())) {
		return false;
	}
	wt::WtChunkPageSampleSource source(pages.front());
	if (source.status() != wt::WtChunkPageSampleSourceStatus::Ok) {
		return false;
	}
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		const wt::WtChunkFace face = static_cast<wt::WtChunkFace>(face_index);
		if ((transition_mask & wt::wt_face_bit(face)) == 0) {
			continue;
		}
		std::array<wt::WtChunkKey, wt::kWtTransitionSupportPagesPerFace> support_keys{};
		if (!wt::wt_transition_support_page_keys(key, face, support_keys)) {
			return false;
		}
		for (const wt::WtChunkKey &support_key : support_keys) {
			pages.emplace_back();
			if (!try_generate_procedural_page(descriptor, support_key, pages.back())) {
				return false;
			}
			if (source.add_transition_support_page(pages.back()) !=
					wt::WtChunkPageSampleSourceStatus::Ok) {
				return false;
			}
		}
	}
	if (!source.has_transition_support(transition_mask)) {
		return false;
	}
	return mesher.mesh(
			{ key, transition_mask, 0.0F, 0.25F },
			source,
			result,
			scratch
		) == wt::WtChunkMeshingStatus::Ok;
}

wt::WtChunkMeshResult mesh_procedural_page_with_transition_support(
	const wt::WtChunkMesher &mesher,
	wt::WtChunkMeshingScratch &scratch,
	const wt::WtProceduralWorldDescriptor &descriptor,
	const wt::WtChunkKey &key,
	std::uint8_t transition_mask
) {
	wt::WtChunkMeshResult result;
	check(try_mesh_procedural_page_with_transition_support(
			mesher,
			scratch,
			descriptor,
			key,
			transition_mask,
			result
		),
		"streaming pixel repro transition mesh failed");
	validate_buffer(result.regular, "invalid streaming pixel repro regular mesh");
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		validate_buffer(result.transitions[face_index],
			"invalid streaming pixel repro transition mesh");
	}
	return result;
}

void check_mesh_winding_matches_normals(
	const wt::WtChunkMeshBuffer &mesh,
	const char *message
) {
	std::size_t negative = 0;
	for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
		if (mesh_triangle_normal_alignment(mesh, index) < -0.0001) {
			++negative;
		}
	}
	check(negative == 0, message);
}

void test_streaming_pixel_transition_repro(std::vector<std::uint8_t> &evidence) {
	const wt::WtChunkMesher mesher(wt::wt_get_transvoxel_mit_backend());
	wt::WtChunkMeshingScratch scratch;
	const wt::WtProceduralWorldDescriptor descriptor =
		human_boundary_repro_descriptor();
	const wt::WtChunkKey coarse_key = { 16, 1, 17, 2 };
	const wt::WtChunkFace face = wt::WtChunkFace::NegativeZ;
	const std::uint8_t mask = wt::wt_face_bit(face);
	wt::WtChunkMeshResult coarse = mesh_procedural_page_with_transition_support(
		mesher,
		scratch,
		descriptor,
		coarse_key,
		mask
	);
	const std::size_t face_index = static_cast<std::size_t>(face);
	check(!coarse.transitions[face_index].indices.empty(),
		"streaming pixel repro transition face is empty");
	check_mesh_winding_matches_normals(
		coarse.regular,
		"streaming pixel repro coarse regular winding mismatch"
	);
	check_mesh_winding_matches_normals(
		coarse.transitions[face_index],
		"streaming pixel repro coarse transition winding mismatch"
	);
	const wt::WtChunkBounds coarse_bounds = wt::wt_chunk_bounds(coarse_key);
	const int axis = face_axis(face);
	const double full_plane = positive_face(face) ?
		static_cast<double>(coarse_bounds.maximum.z) :
		static_cast<double>(coarse_bounds.minimum.z);
	const double width =
		static_cast<double>(wt::wt_lod_cell_size(coarse_key.lod)) * 0.25;
	const double half_plane = positive_face(face) ?
		full_plane - width :
		full_plane + width;
	const EdgeSet transition_full = plane_boundary_edges(
		coarse.transitions[face_index],
		coarse.world_origin,
		axis,
		full_plane
	);
	const EdgeSet transition_half = plane_boundary_edges(
		coarse.transitions[face_index],
		coarse.world_origin,
		axis,
		half_plane
	);
	const EdgeSet coarse_regular = plane_boundary_edges(
		coarse.regular,
		coarse.world_origin,
		axis,
		half_plane
	);
	check(!transition_full.empty(),
		"streaming pixel repro transition full contour is empty");
	check(transition_half == coarse_regular,
		"streaming pixel repro transition half contour does not match coarse regular mesh");

	std::array<wt::WtChunkKey, wt::kWtTransitionSupportPagesPerFace> support_keys{};
	check(wt::wt_transition_support_page_keys(coarse_key, face, support_keys),
		"streaming pixel repro support key query failed");
	EdgeSet fine_edges;
	for (const wt::WtChunkKey &support_key : support_keys) {
		wt::WtChunkPage page = generate_procedural_page(descriptor, support_key);
		wt::WtChunkMeshResult fine = mesh_page_direct(mesher, scratch, page);
		check_mesh_winding_matches_normals(
			fine.regular,
			"streaming pixel repro fine winding mismatch"
		);
		const EdgeSet edges = plane_boundary_edges(
			fine.regular,
			fine.world_origin,
			axis,
			full_plane
		);
		fine_edges.insert(edges.begin(), edges.end());
	}
	std::size_t missing_fine_edges = 0;
	for (const Edge &edge : fine_edges) {
		if (transition_full.find(edge) == transition_full.end()) {
			++missing_fine_edges;
		}
	}
	std::size_t extra_transition_edges = 0;
	for (const Edge &edge : transition_full) {
		if (fine_edges.find(edge) == fine_edges.end()) {
			++extra_transition_edges;
		}
	}
	check(missing_fine_edges == 0,
		"streaming pixel repro transition full contour is missing fine neighbor edges");
	evidence.push_back(static_cast<std::uint8_t>(transition_full.size()));
	evidence.push_back(static_cast<std::uint8_t>(fine_edges.size()));
	evidence.push_back(static_cast<std::uint8_t>(missing_fine_edges));
	evidence.push_back(static_cast<std::uint8_t>(extra_transition_edges));
	evidence.push_back(static_cast<std::uint8_t>(transition_half.size()));
	evidence.push_back(static_cast<std::uint8_t>(coarse_regular.size()));
}

void test_human_boundary_edit_repro(std::vector<std::uint8_t> &evidence) {
	const wt::WtChunkMesher mesher(wt::wt_get_transvoxel_mit_backend());
	wt::WtChunkMeshingScratch scratch;
	const wt::WtProceduralWorldDescriptor descriptor =
		human_boundary_repro_descriptor();
	const std::vector<wt::WtEditCommand> commands =
		human_boundary_repro_commands();
	const std::array<wt::WtChunkKey, 8> keys = {{
		{ 73, 6, 62, 0 }, { 74, 6, 62, 0 },
		{ 73, 7, 62, 0 }, { 74, 7, 62, 0 },
		{ 73, 6, 63, 0 }, { 74, 6, 63, 0 },
		{ 73, 7, 63, 0 }, { 74, 7, 63, 0 },
	}};
	std::uint64_t repro_hash = 14695981039346656037ULL;
	std::map<wt::WtChunkKey, wt::WtChunkMeshResult> meshes;
	for (const wt::WtChunkKey &key : keys) {
		wt::WtChunkPage page = edited_procedural_page(descriptor, key, commands);
		wt::WtChunkMeshResult result = mesh_page_direct(mesher, scratch, page);
		check_mesh_winding_matches_normals(
			result.regular,
			"human boundary repro produced inverted regular triangles"
		);
		hash_result(repro_hash, result);
		meshes.emplace(key, std::move(result));
	}
	bool inspected_nonempty_x_seam = false;
	for (std::int32_t y : { 6, 7 }) {
		for (std::int32_t z : { 62, 63 }) {
			const wt::WtChunkKey left_key = { 73, y, z, 0 };
			const wt::WtChunkKey right_key = { 74, y, z, 0 };
			const wt::WtChunkMeshResult &left = meshes[left_key];
			const wt::WtChunkMeshResult &right = meshes[right_key];
			const EdgeSet left_edges = plane_boundary_edges(
				left.regular, left.world_origin, 0, 1184.0
			);
			const EdgeSet right_edges = plane_boundary_edges(
				right.regular, right.world_origin, 0, 1184.0
			);
			if (!left_edges.empty() || !right_edges.empty()) {
				inspected_nonempty_x_seam = true;
			}
			check(left_edges == right_edges,
				"human boundary repro same-LOD x seam mismatch");
		}
	}
	check(inspected_nonempty_x_seam,
		"human boundary repro did not inspect any x-face seam edges");
	for (unsigned int shift = 0; shift < 64; shift += 8) {
		evidence.push_back(static_cast<std::uint8_t>(repro_hash >> shift));
	}
}

bool write_file(
	const std::filesystem::path &path,
	const std::vector<std::uint8_t> &bytes
) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) {
		return false;
	}
	if (!bytes.empty()) {
		output.write(
			reinterpret_cast<const char *>(bytes.data()),
			static_cast<std::streamsize>(bytes.size())
		);
	}
	return static_cast<bool>(output);
}

std::vector<wt::WtDependencyEntry> fixture_dependencies() {
	return {
		dependency(wt::WtDependencyKind::SourceAsset,
			"runtime-density", "", "runtime-density-bytes"),
		dependency(wt::WtDependencyKind::Generator,
			"world-transvoxel-baker", "0.5.0-m4", "runtime-generator"),
		dependency(wt::WtDependencyKind::Configuration,
			"m5-page-runtime", "1", "runtime-configuration"),
		dependency(wt::WtDependencyKind::Backend,
			"transvoxel-mit", "fixture", "runtime-backend"),
		dependency(wt::WtDependencyKind::Godot,
			"godot", "4.6.3", "runtime-godot"),
		dependency(wt::WtDependencyKind::GodotCpp,
			"godot-cpp", "e83fd090", "runtime-godot-cpp"),
		dependency(wt::WtDependencyKind::Toolchain,
			"zig", "0.16.0", "runtime-zig"),
	};
}

struct RuntimeFixture {
	std::filesystem::path root;
	std::filesystem::path world_path;
	std::filesystem::path incomplete_world_path;
	wt::WtChunkKey coarse_key = { 0, 0, 0, 1 };
	std::uint8_t transition_mask = 0;
	std::vector<wt::WtChunkKey> support_keys;
	std::vector<wt::WtBakedChunkPage> pages;

	RuntimeFixture() = default;
	RuntimeFixture(const RuntimeFixture &) = delete;
	RuntimeFixture &operator=(const RuntimeFixture &) = delete;
	RuntimeFixture(RuntimeFixture &&other) noexcept :
			root(std::move(other.root)),
			world_path(std::move(other.world_path)),
			incomplete_world_path(std::move(other.incomplete_world_path)),
			coarse_key(other.coarse_key),
			transition_mask(other.transition_mask),
			support_keys(std::move(other.support_keys)),
			pages(std::move(other.pages)) {
		other.root.clear();
	}

	~RuntimeFixture() {
		if (root.empty()) {
			return;
		}
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}
};

bool write_manifest(
	const RuntimeFixture &fixture,
	const std::filesystem::path &path,
	const wt::WtChunkKey *omitted_key
) {
	wt::WtWorldManifest manifest;
	manifest.source_revision = kSourceRevision;
	manifest.world_revision = kWorldRevision;
	manifest.configuration_hash = hash_text("runtime-configuration");
	manifest.dependencies = fixture_dependencies();
	for (const wt::WtBakedChunkPage &page : fixture.pages) {
		if (omitted_key != nullptr && page.key == *omitted_key) {
			continue;
		}
		manifest.pages.push_back({
			page.key,
			page.bytes.size(),
			page.content_hash,
		});
	}
	std::vector<std::uint8_t> bytes;
	return wt::wt_write_world_manifest(manifest, bytes) ==
			wt::WtWorldManifestStatus::Ok &&
		write_file(path, bytes);
}

RuntimeFixture make_fixture() {
	RuntimeFixture fixture;
	fixture.root = std::filesystem::temp_directory_path() /
		("wt_m5_page_runtime_" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()
		));
	std::error_code error;
	check(
		std::filesystem::create_directories(fixture.root, error) && !error,
		"runtime fixture directory creation failed"
	);

	fixture.transition_mask = static_cast<std::uint8_t>(
		wt::wt_face_bit(wt::WtChunkFace::PositiveX) |
		wt::wt_face_bit(wt::WtChunkFace::PositiveY) |
		wt::wt_face_bit(wt::WtChunkFace::PositiveZ)
	);
	std::set<wt::WtChunkKey> unique_support;
	for (wt::WtChunkFace face : {
			wt::WtChunkFace::PositiveX,
			wt::WtChunkFace::PositiveY,
			wt::WtChunkFace::PositiveZ,
		}) {
		std::array<wt::WtChunkKey, wt::kWtTransitionSupportPagesPerFace>
			support{};
		check(
			wt::wt_transition_support_page_keys(
				fixture.coarse_key,
				face,
				support
			),
			"runtime support key generation failed"
		);
		unique_support.insert(support.begin(), support.end());
	}
	fixture.support_keys.assign(unique_support.begin(), unique_support.end());
	std::vector<wt::WtChunkKey> keys = { fixture.coarse_key };
	keys.insert(keys.end(), fixture.support_keys.begin(), fixture.support_keys.end());
	const SphereSource source;
	wt::WtChunkBaker baker(keys.size());
	check(
		baker.bake(keys, kSourceRevision, source, fixture.pages) ==
			wt::WtChunkBakeStatus::Ok,
		"runtime fixture page bake failed"
	);
	for (const wt::WtBakedChunkPage &page : fixture.pages) {
		check(
			write_file(
				wt::wt_page_object_path(fixture.root, page.content_hash),
				page.bytes
			),
			"runtime fixture page write failed"
		);
	}
	fixture.world_path = fixture.root / "world.wtworld";
	fixture.incomplete_world_path = fixture.root / "incomplete.wtworld";
	check(
		write_manifest(fixture, fixture.world_path, nullptr),
		"runtime fixture manifest write failed"
	);
	check(
		!fixture.support_keys.empty() &&
		write_manifest(
			fixture,
			fixture.incomplete_world_path,
			&fixture.support_keys.front()
		),
		"incomplete runtime fixture manifest write failed"
	);
	return fixture;
}

wt::WtChunkJob request_sample_job(
	wt::WtStreamScheduler &scheduler,
	const wt::WtChunkKey &key,
	std::uint64_t world_revision,
	std::int32_t priority
) {
	wt::WtChunkJob job;
	check(
		scheduler.request_chunk_version(
			key,
			kSourceRevision,
			world_revision,
			priority
		) == wt::WtSchedulerStatus::Ok &&
		scheduler.pop_job(job) &&
		job.stage == wt::WtChunkJobStage::Sample,
		"sample job request failed"
	);
	return job;
}

bool wait_completion(
	wt::WtAsyncStorageService &storage,
	wt::WtPageLoadCompletion &completion
) {
	const bool completed = storage.wait_pop_completion(
		completion,
		std::chrono::seconds(3)
	);
	check(completed, "timed out waiting for runtime page");
	return completed;
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
	for (std::size_t index = 0; index < 8; ++index) {
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
	}
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void test_runtime_lifecycle(
	const RuntimeFixture &fixture,
	std::vector<std::uint8_t> &evidence
) {
	wt::WtAsyncStorageService storage({ 32, 32, wt::kWtMaximumContainerSize });
	check(
		storage.open(fixture.world_path, fixture.root) ==
			wt::WtAsyncStorageStatus::Ok,
		"runtime storage open failed"
	);
	wt::WtStoragePageCache cache({
		2,
		wt::kWtMaximumContainerSize,
		2,
		wt::kWtMaximumContainerSize,
	});
	wt::WtStreamScheduler scheduler(8, 8, 1, 1);
	wt::WtPageMeshingRuntimeService runtime(8);
	const wt::WtChunkJob sample = request_sample_job(
		scheduler,
		fixture.coarse_key,
		kWorldRevision,
		7
	);
	check(
		runtime.begin_sample_job(
			sample,
			fixture.transition_mask,
			storage,
			cache,
			scheduler
		) == wt::WtPageMeshingRuntimeStatus::Ok,
		"page runtime rejected sample job"
	);
	const auto loading = runtime.get_records();
	check(
		loading.size() == 1 &&
		loading[0].phase == wt::WtPageMeshingRuntimePhase::Loading &&
		loading[0].dependency_count == kDependencyCount,
		"runtime loading record mismatch"
	);
	check(
		runtime.reprioritize_owned_chunk(
			fixture.coarse_key,
			sample.generation,
			11
		) == wt::WtPageMeshingRuntimeOwnerStatus::Ok &&
		runtime.resume_loading_records(storage, cache, scheduler, 1) == 1 &&
		storage.get_metrics().duplicate_requests == kDependencyCount,
		"loading dependency repriority did not reach queued storage requests"
	);
	check(
		scheduler.submit_completion({
			{ 99, 99, 99, 0 },
			{ 999 },
			wt::WtChunkJobStage::Sample,
			true,
		}) == wt::WtSchedulerStatus::Ok,
		"scheduler backpressure fixture failed"
	);

	std::size_t backpressure_count = 0;
	for (std::size_t index = 0; index < kDependencyCount; ++index) {
		wt::WtPageLoadCompletion completion;
		if (!wait_completion(storage, completion)) {
			break;
		}
		const wt::WtPageMeshingRuntimeStatus status =
			runtime.accept_storage_completion(completion, cache, scheduler);
		if (status == wt::WtPageMeshingRuntimeStatus::SchedulerBackpressure) {
			++backpressure_count;
		} else {
			check(
				status == wt::WtPageMeshingRuntimeStatus::Ok,
				"runtime rejected valid storage completion"
			);
		}
	}
	const auto loaded = runtime.get_records();
	check(
		backpressure_count == 1 && loaded.size() == 1 &&
		loaded[0].phase == wt::WtPageMeshingRuntimePhase::SampleReady &&
		loaded[0].pinned_page_count == kDependencyCount &&
		runtime.pinned_page_count() == kDependencyCount &&
		cache.decoded_entry_count() == 2,
		"runtime pin/backpressure contract mismatch"
	);
	check(
		runtime.reprioritize_owned_chunk(
			fixture.coarse_key,
			{ sample.generation.value + 1 },
			12
		) == wt::WtPageMeshingRuntimeOwnerStatus::StaleGeneration &&
		runtime.get_records()[0].priority == 11,
		"runtime owner repriority/generation guard mismatch"
	);
	check(
		scheduler.apply_completions(1) == 1 &&
		runtime.flush_scheduler_results(scheduler) == 1 &&
		scheduler.apply_completions(1) == 1,
		"sample completion retry failed"
	);

	wt::WtChunkJob mesh_job;
	check(
		scheduler.pop_job(mesh_job) &&
		mesh_job.stage == wt::WtChunkJobStage::Mesh,
		"runtime mesh job was not scheduled"
	);
	const wt::WtChunkMesher mesher(wt::wt_get_transvoxel_mit_backend());
	wt::WtChunkMeshingScratch scratch;
	bool terrain_ready_before_water = false;
	check(
		runtime.execute_mesh_job(
			mesh_job,
			mesher,
			scratch,
			scheduler,
			nullptr,
			0,
			nullptr,
			[&](const wt::WtTerrainMeshCompletion &completion) {
				const auto records = runtime.get_records();
				terrain_ready_before_water =
					completion.key == fixture.coarse_key &&
					completion.generation == sample.generation &&
					completion.mesh != nullptr && records.size() == 1 &&
					records[0].phase ==
						wt::WtPageMeshingRuntimePhase::AwaitingMesh &&
					records[0].pinned_page_count == kDependencyCount;
				return terrain_ready_before_water;
			}
		) ==
			wt::WtPageMeshingRuntimeStatus::Ok,
		"page-backed runtime meshing failed"
	);
	const auto meshed = runtime.get_records();
	check(
		terrain_ready_before_water && meshed.size() == 1 &&
		meshed[0].phase == wt::WtPageMeshingRuntimePhase::Ready &&
		meshed[0].pinned_page_count == 0 &&
		runtime.pinned_page_count() == 0,
		"runtime did not release page pins after meshing"
	);
	check(
		scheduler.apply_completions(1) == 1 &&
		scheduler.find_record(fixture.coarse_key) != nullptr &&
		scheduler.find_record(fixture.coarse_key)->lifecycle ==
			wt::WtChunkLifecycle::Ready,
		"scheduler did not accept runtime mesh completion"
	);
	wt::WtPageMeshCompletion mesh_completion;
	check(
		runtime.pop_mesh_completion(mesh_completion) &&
		mesh_completion.key == fixture.coarse_key &&
		mesh_completion.generation == sample.generation &&
		mesh_completion.mesh != nullptr,
		"runtime mesh completion ownership mismatch"
	);
	std::uint64_t mesh_hash = 14695981039346656037ULL;
	if (mesh_completion.mesh) {
		validate_buffer(
			mesh_completion.mesh->regular,
			"invalid runtime regular mesh"
		);
		std::size_t transition_indices = 0;
		for (std::size_t face = 0; face < 6; ++face) {
			transition_indices +=
				mesh_completion.mesh->transitions[face].indices.size();
		}
		check(
			transition_indices != 0,
			"runtime transition mesh is empty"
		);
		hash_result(mesh_hash, *mesh_completion.mesh);
	}

	std::vector<wt::WtPageMeshingInvalidation> invalidated;
	check(
		runtime.invalidate_dependency(
			fixture.support_keys.front(),
			invalidated
		) == wt::WtPageMeshingRuntimeStatus::Ok &&
		invalidated.size() == 1 &&
		invalidated[0].key == fixture.coarse_key &&
		invalidated[0].generation == sample.generation &&
		runtime.record_count() == 0,
		"support-page invalidation did not retire the coarse generation"
	);
	check(
		scheduler.cancel_chunk(fixture.coarse_key) ==
			wt::WtSchedulerStatus::Ok &&
		scheduler.forget_chunk(fixture.coarse_key) ==
			wt::WtSchedulerStatus::Ok,
		"invalidation scheduler cleanup failed"
	);

	wt::WtStoragePageCache cancellation_cache({
		2,
		wt::kWtMaximumContainerSize,
		2,
		wt::kWtMaximumContainerSize,
	});
	const wt::WtChunkJob cancelled = request_sample_job(
		scheduler,
		fixture.coarse_key,
		kWorldRevision + 1,
		3
	);
	check(
		runtime.begin_sample_job(
			cancelled,
			fixture.transition_mask,
			storage,
			cancellation_cache,
			scheduler
		) == wt::WtPageMeshingRuntimeStatus::Ok &&
		runtime.cancel_owned_generation(
			fixture.coarse_key,
			cancelled.generation
		) == wt::WtPageMeshingRuntimeOwnerStatus::Ok &&
		scheduler.cancel_chunk(fixture.coarse_key) ==
			wt::WtSchedulerStatus::Ok,
		"runtime generation cancellation failed"
	);
	std::size_t stale_count = 0;
	for (std::size_t index = 0; index < kDependencyCount; ++index) {
		wt::WtPageLoadCompletion completion;
		if (!wait_completion(storage, completion)) {
			break;
		}
		stale_count += runtime.accept_storage_completion(
			completion,
			cancellation_cache,
			scheduler
		) == wt::WtPageMeshingRuntimeStatus::CompletionNotOwned ? 1U : 0U;
	}
	check(
		stale_count == kDependencyCount &&
		runtime.record_count() == 0 &&
		runtime.pinned_page_count() == 0 &&
		scheduler.forget_chunk(fixture.coarse_key) ==
			wt::WtSchedulerStatus::Ok,
		"late cancelled completions mutated runtime state"
	);

	const wt::WtPageMeshingRuntimeMetrics metrics = runtime.get_metrics();
	check(
		metrics.sample_jobs == 2 && metrics.mesh_jobs == 1 &&
		metrics.dependency_requests == kDependencyCount * 2 &&
		metrics.accepted_storage_completions == kDependencyCount &&
		metrics.stale_storage_completions == kDependencyCount &&
		metrics.sample_successes == 1 && metrics.mesh_successes == 1 &&
		metrics.scheduler_backpressure == 1 &&
		metrics.cancellations == 1 &&
		metrics.invalidated_records == 1 &&
		metrics.maximum_pinned_pages == kDependencyCount,
		"page runtime metrics mismatch"
	);
	append_u64(evidence, mesh_hash);
	append_u64(evidence, metrics.dependency_requests);
	append_u64(evidence, metrics.accepted_storage_completions);
	append_u64(evidence, metrics.stale_storage_completions);
	append_u64(evidence, metrics.scheduler_backpressure);
	append_u64(evidence, metrics.maximum_pinned_pages);
	storage.close();
}

void test_edited_coarse_procedural_rebuild(
	std::vector<std::uint8_t> &evidence
) {
	wt::WtProceduralWorldDescriptor descriptor;
	descriptor.chunk_count_x = 8;
	descriptor.chunk_count_y = 8;
	descriptor.chunk_count_z = 8;
	descriptor.source_revision = 7201;
	descriptor.world_revision = 0;
	descriptor.seed = 17;
	descriptor.mode = wt::WtProceduralWorldMode::Flat;
	wt::WtAsyncStorageService storage({ 4, 4, wt::kWtMaximumContainerSize });
	check(
		storage.open_procedural(descriptor) == wt::WtAsyncStorageStatus::Ok,
		"edited coarse procedural storage open failed"
	);

	wt::WtEditCommand command;
	command.command_id = repro_id(1000);
	command.sequence = 0;
	command.world_revision = 1;
	command.operation = wt::WtEditOperation::AddDensity;
	command.shape = wt::WtEditShape::Sphere;
	command.density_value = 20.0F;
	command.sphere = { q16(8.0), q16(8.0), q16(8.0), uq16(4.0) };
	check(
		wt::wt_edit_sphere_bounds(command.sphere, command.bounds),
		"edited coarse procedural bounds failed"
	);
	wt::WtEditTransaction transaction;
	transaction.source_revision = descriptor.source_revision;
	transaction.transaction_id = repro_id(1001);
	transaction.base_revision = 0;
	transaction.committed_revision = 1;
	transaction.commands = { command };
	wt::WtEditJournal journal(1, 1, 4096);
	journal.reset(descriptor.source_revision, 0);
	std::vector<std::uint8_t> journal_segment;
	check(
		journal.append(transaction, journal_segment) ==
			wt::WtEditJournalStatus::Ok,
		"edited coarse procedural journal append failed"
	);

	const wt::WtChunkKey key = { 0, 0, 0, 1 };
	wt::WtStoragePageCache cache({
		4,
		wt::kWtMaximumContainerSize,
		4,
		wt::kWtMaximumContainerSize,
	});
	wt::WtStreamScheduler scheduler(4, 4, 4, 1);
	wt::WtPageMeshingRuntimeService runtime(4);
	check(
		scheduler.request_chunk_version(
			key,
			descriptor.source_revision,
			1,
			9
		) == wt::WtSchedulerStatus::Ok,
		"edited coarse procedural request failed"
	);
	wt::WtChunkJob sample_job;
	check(
		scheduler.pop_job(sample_job) &&
			sample_job.stage == wt::WtChunkJobStage::Sample &&
			runtime.begin_sample_job(
				sample_job,
				0,
				storage,
				cache,
				scheduler
			) == wt::WtPageMeshingRuntimeStatus::Ok,
		"edited coarse procedural sample start failed"
	);
	wt::WtPageLoadCompletion page_completion;
	check(
		wait_completion(storage, page_completion) &&
			runtime.accept_storage_completion(
				page_completion, cache, scheduler
			) == wt::WtPageMeshingRuntimeStatus::Ok &&
			scheduler.apply_completions(1) == 1,
		"edited coarse procedural sample completion failed"
	);
	wt::WtChunkJob mesh_job;
	check(
		scheduler.pop_job(mesh_job) &&
			mesh_job.stage == wt::WtChunkJobStage::Mesh,
		"edited coarse procedural mesh job missing"
	);
	const wt::WtChunkMesher mesher(wt::wt_get_transvoxel_mit_backend());
	wt::WtChunkMeshingScratch scratch;
	check(
		runtime.execute_mesh_job(
			mesh_job,
			mesher,
			scratch,
			scheduler,
			&journal,
			0,
			&storage
		) == wt::WtPageMeshingRuntimeStatus::Ok,
		"edited coarse procedural surface-shift rebuild failed"
	);
	const wt::WtPageMeshingRuntimeMetrics metrics = runtime.get_metrics();
	check(
		metrics.surface_shift_rebuilds == 1 &&
			metrics.surface_shift_failures == 0 &&
			metrics.mesh_successes == 1,
		"edited coarse procedural rebuild metrics mismatch"
	);
	check(
		scheduler.apply_completions(1) == 1,
		"edited coarse procedural mesh completion failed"
	);
	wt::WtPageMeshCompletion mesh_completion;
	check(
		runtime.pop_mesh_completion(mesh_completion) &&
			mesh_completion.mesh != nullptr &&
			!mesh_completion.mesh->regular.indices.empty(),
		"edited coarse procedural mesh result missing"
	);
	std::uint64_t mesh_hash = 14695981039346656037ULL;
	if (mesh_completion.mesh) {
		hash_result(mesh_hash, *mesh_completion.mesh);
	}
	append_u64(evidence, mesh_hash);
	append_u64(evidence, metrics.surface_shift_rebuilds);
	storage.close();
}

void test_storage_backpressure_retry(const RuntimeFixture &fixture) {
	wt::WtAsyncStorageService storage({ 2, 32, wt::kWtMaximumContainerSize });
	check(
		storage.open(fixture.world_path, fixture.root) ==
			wt::WtAsyncStorageStatus::Ok,
		"backpressure retry storage open failed"
	);
	wt::WtStoragePageCache cache({
		2,
		wt::kWtMaximumContainerSize,
		2,
		wt::kWtMaximumContainerSize,
	});
	wt::WtStreamScheduler scheduler(4, 4, 4, 1);
	wt::WtPageMeshingRuntimeService runtime(4);
	const wt::WtChunkJob sample = request_sample_job(
		scheduler,
		fixture.coarse_key,
		kWorldRevision + 3,
		5
	);
	const wt::WtPageMeshingRuntimeStatus begin_status =
		runtime.begin_sample_job(
			sample,
			fixture.transition_mask,
			storage,
			cache,
			scheduler
		);
	check(
		begin_status == wt::WtPageMeshingRuntimeStatus::SchedulerBackpressure ||
			begin_status == wt::WtPageMeshingRuntimeStatus::Ok,
		"storage queue backpressure was treated as a sample failure"
	);
	for (std::size_t attempt = 0; attempt < 64; ++attempt) {
		const auto records = runtime.get_records();
		if (!records.empty() &&
			records[0].phase == wt::WtPageMeshingRuntimePhase::AwaitingMesh) {
			break;
		}
		wt::WtPageLoadCompletion completion;
		if (!wait_completion(storage, completion)) {
			break;
		}
		const wt::WtPageMeshingRuntimeStatus completion_status =
			runtime.accept_storage_completion(completion, cache, scheduler);
		check(
			completion_status == wt::WtPageMeshingRuntimeStatus::Ok ||
				completion_status ==
					wt::WtPageMeshingRuntimeStatus::SchedulerBackpressure,
			"backpressure retry rejected a storage completion"
		);
		runtime.resume_loading_records(storage, cache, scheduler, 1);
		runtime.flush_scheduler_results(scheduler);
	}
	const auto records = runtime.get_records();
	const wt::WtPageMeshingRuntimeMetrics metrics = runtime.get_metrics();
	check(
		records.size() == 1 &&
			records[0].phase == wt::WtPageMeshingRuntimePhase::AwaitingMesh &&
			records[0].pinned_page_count == kDependencyCount &&
			metrics.sample_failures == 0 &&
			metrics.storage_failures == 0 &&
			metrics.dependency_requests == kDependencyCount,
		"storage backpressure retry did not reach sample-ready state"
	);
	check(
		scheduler.apply_completions(1) == 1 &&
			scheduler.find_record(fixture.coarse_key) != nullptr &&
			scheduler.find_record(fixture.coarse_key)->lifecycle ==
				wt::WtChunkLifecycle::Meshing,
		"backpressure retry sample completion was not applied"
	);
	storage.close();
}

void test_missing_support(const RuntimeFixture &fixture) {
	wt::WtAsyncStorageService storage({ 32, 32, wt::kWtMaximumContainerSize });
	check(
		storage.open(fixture.incomplete_world_path, fixture.root) ==
			wt::WtAsyncStorageStatus::Ok,
		"incomplete runtime storage open failed"
	);
	wt::WtStoragePageCache cache({
		2,
		wt::kWtMaximumContainerSize,
		2,
		wt::kWtMaximumContainerSize,
	});
	wt::WtStreamScheduler scheduler(2, 2, 2, 1);
	wt::WtPageMeshingRuntimeService runtime(2);
	const wt::WtChunkJob sample = request_sample_job(
		scheduler,
		fixture.coarse_key,
		kWorldRevision,
		0
	);
	check(
		runtime.begin_sample_job(
			sample,
			fixture.transition_mask,
			storage,
			cache,
			scheduler
		) == wt::WtPageMeshingRuntimeStatus::StorageRequestFailure &&
		runtime.record_count() == 0 &&
		runtime.get_metrics().sample_failures == 1 &&
		scheduler.apply_completions(1) == 1 &&
		scheduler.find_record(fixture.coarse_key) != nullptr &&
		scheduler.find_record(fixture.coarse_key)->lifecycle ==
			wt::WtChunkLifecycle::Failed,
		"missing transition support did not fail the generation"
	);
	storage.close();

	wt::WtPageMeshingRuntimeService invalid(0);
	check(!invalid.valid(), "zero page runtime capacity was accepted");
	wt::WtChunkJob lod0_job = sample;
	lod0_job.key = { 0, 0, 0, 0 };
	check(
		runtime.begin_sample_job(
			lod0_job,
			wt::wt_face_bit(wt::WtChunkFace::PositiveX),
			storage,
			cache,
			scheduler
		) == wt::WtPageMeshingRuntimeStatus::InvalidConfiguration,
		"closed storage did not reject runtime work"
	);
}

} // namespace

int main() {
	RuntimeFixture fixture = make_fixture();
	check(
		fixture.pages.size() == kDependencyCount &&
		fixture.support_keys.size() == kDependencyCount - 1,
		"runtime fixture dependency count mismatch"
	);
	std::vector<std::uint8_t> evidence;
	test_representable_sphere_difference_topology(evidence);
	test_material_volume_continuous_distance(evidence);
	test_four_biome_water_free_surface(evidence);
	test_human_boundary_edit_repro(evidence);
	test_streaming_pixel_transition_repro(evidence);
	test_runtime_lifecycle(fixture, evidence);
	test_edited_coarse_procedural_rebuild(evidence);
	test_storage_backpressure_retry(fixture);
	test_missing_support(fixture);
	if (failure_count != 0) {
		std::fprintf(stderr, "M5_PAGE_MESHING_RUNTIME_FAIL failures=%d\n",
			failure_count);
		return 1;
	}
	std::printf("M5_PAGE_MESHING_RUNTIME_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"M5_PAGE_MESHING_RUNTIME_PASS dependencies=13 cache_entries=2 "
		"backpressure=1 cancellations=1 invalidations=1 missing_support=1 "
		"human_boundary_repro=1 edited_coarse_rebuild=1 "
		"sphere_difference_topology=1 smooth_sphere_difference=1 "
		"material_volume_distance=1 water_lod_footprint=1\n"
	);
	return 0;
}
