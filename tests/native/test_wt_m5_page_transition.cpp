#include "backend/wt_transvoxel_mit_backend.h"
#include "bake/wt_chunk_baker.h"
#include "storage/wt_chunk_page_sample_source.h"
#include "wt_m2_mesh_test_support.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>
#include <vector>

namespace wt = world_transvoxel;
using namespace world_transvoxel_test;

namespace {

int face_axis(wt::WtChunkFace face) {
	return static_cast<int>(face) / 2;
}

bool positive_face(wt::WtChunkFace face) {
	return (static_cast<unsigned int>(face) & 1U) != 0U;
}

std::int64_t axis_coordinate(
	const wt::WtGridPoint &point,
	int axis
) {
	if (axis == 0) return point.x;
	if (axis == 1) return point.y;
	return point.z;
}

double face_threshold(
	const wt::WtChunkKey &coarse_key,
	wt::WtChunkFace face
) {
	const wt::WtChunkBounds bounds = wt::wt_chunk_bounds(coarse_key);
	double coordinates[3] = {
		static_cast<double>(bounds.minimum.x + bounds.maximum.x) * 0.5,
		static_cast<double>(bounds.minimum.y + bounds.maximum.y) * 0.5,
		static_cast<double>(bounds.minimum.z + bounds.maximum.z) * 0.5,
	};
	const int axis = face_axis(face);
	coordinates[axis] = static_cast<double>(
		positive_face(face) ?
			axis_coordinate(bounds.maximum, axis) :
			axis_coordinate(bounds.minimum, axis)
	);
	return 0.37 * coordinates[0] +
		0.53 * coordinates[1] +
		0.71 * coordinates[2] +
		0.123;
}

bool decode_pages(
	const std::vector<wt::WtBakedChunkPage> &baked,
	std::vector<wt::WtChunkPage> &decoded
) {
	decoded.clear();
	decoded.reserve(baked.size());
	for (const wt::WtBakedChunkPage &entry : baked) {
		wt::WtChunkPageView view;
		wt::WtChunkPage page;
		if (wt::wt_open_chunk_page(
				{ entry.bytes.data(), entry.bytes.size() },
				view
			) != wt::WtChunkPageStatus::Ok ||
			wt::wt_decode_chunk_page(view, page) !=
				wt::WtChunkPageStatus::Ok) {
			return false;
		}
		decoded.push_back(std::move(page));
	}
	return true;
}

const wt::WtChunkPage *find_page(
	const std::vector<wt::WtChunkPage> &pages,
	const wt::WtChunkKey &key
) {
	for (const wt::WtChunkPage &page : pages) {
		if (page.metadata.key == key) {
			return &page;
		}
	}
	return nullptr;
}

std::uint64_t mesh_hash(const wt::WtChunkMeshResult &result) {
	std::uint64_t hash = 14695981039346656037ULL;
	hash_result(hash, result);
	return hash;
}

void test_support_keys() {
	const wt::WtChunkKey key = { -2, 3, -4, 2 };
	const std::array<std::array<wt::WtChunkKey, 4>, 6> expected = {{
		{{ { -5, 6, -8, 1 }, { -5, 6, -7, 1 },
			{ -5, 7, -8, 1 }, { -5, 7, -7, 1 } }},
		{{ { -2, 6, -8, 1 }, { -2, 6, -7, 1 },
			{ -2, 7, -8, 1 }, { -2, 7, -7, 1 } }},
		{{ { -4, 5, -8, 1 }, { -4, 5, -7, 1 },
			{ -3, 5, -8, 1 }, { -3, 5, -7, 1 } }},
		{{ { -4, 8, -8, 1 }, { -4, 8, -7, 1 },
			{ -3, 8, -8, 1 }, { -3, 8, -7, 1 } }},
		{{ { -4, 6, -9, 1 }, { -4, 7, -9, 1 },
			{ -3, 6, -9, 1 }, { -3, 7, -9, 1 } }},
		{{ { -4, 6, -6, 1 }, { -4, 7, -6, 1 },
			{ -3, 6, -6, 1 }, { -3, 7, -6, 1 } }},
	}};
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		std::array<wt::WtChunkKey, 4> actual{};
		check(
			wt::wt_transition_support_page_keys(
				key,
				static_cast<wt::WtChunkFace>(face_index),
				actual
			) &&
			actual == expected[face_index],
			"transition support key mapping mismatch"
		);
	}
	std::array<wt::WtChunkKey, 4> rejected{};
	check(
		!wt::wt_transition_support_page_keys(
			{ 0, 0, 0, 0 },
			wt::WtChunkFace::NegativeX,
			rejected
		),
		"LOD0 transition support was accepted"
	);
	check(
		!wt::wt_transition_support_page_keys(
			{ std::numeric_limits<std::int32_t>::max(), 0, 0, 1 },
			wt::WtChunkFace::PositiveX,
			rejected
		),
		"overflowing transition support was accepted"
	);
}

void run_face_gallery(
	wt::WtChunkFace face,
	const wt::WtChunkMesher &mesher,
	wt::WtChunkMeshingScratch &scratch,
	std::uint64_t &hash
) {
	const wt::WtChunkKey coarse_key = { 0, 0, 0, 2 };
	std::array<wt::WtChunkKey, 4> support_keys{};
	check(
		wt::wt_transition_support_page_keys(
			coarse_key,
			face,
			support_keys
		),
		"face support key generation failed"
	);
	std::vector<wt::WtChunkKey> keys = { coarse_key };
	keys.insert(keys.end(), support_keys.begin(), support_keys.end());
	LinearSource source;
	source.threshold = face_threshold(coarse_key, face);
	wt::WtChunkBaker baker(keys.size());
	std::vector<wt::WtBakedChunkPage> baked;
	std::vector<wt::WtChunkPage> pages;
	check(
		baker.bake(keys, 71, source, baked) ==
			wt::WtChunkBakeStatus::Ok &&
		decode_pages(baked, pages),
		"face page bake/decode failed"
	);
	const wt::WtChunkPage *coarse_page = find_page(pages, coarse_key);
	check(coarse_page != nullptr, "coarse page missing");
	if (coarse_page == nullptr) {
		return;
	}
	wt::WtChunkPageSampleSource coarse_source(*coarse_page);
	for (const wt::WtChunkKey &key : support_keys) {
		const wt::WtChunkPage *support = find_page(pages, key);
		check(
			support != nullptr &&
			coarse_source.add_transition_support_page(*support) ==
				wt::WtChunkPageSampleSourceStatus::Ok,
			"transition support page rejected"
		);
	}
	check(
		coarse_source.has_transition_support(face) &&
		coarse_source.has_transition_support(wt::wt_face_bit(face)),
		"complete transition support was not recognized"
	);

	wt::WtChunkMeshResult coarse;
	check(
		mesher.mesh(
			{ coarse_key, wt::wt_face_bit(face), 0.0F, 0.25F },
			coarse_source,
			coarse,
			scratch
		) == wt::WtChunkMeshingStatus::Ok,
		"decoded coarse transition mesh failed"
	);
	const std::size_t face_index = static_cast<std::size_t>(face);
	validate_buffer(coarse.regular, "invalid decoded coarse regular mesh");
	validate_buffer(
		coarse.transitions[face_index],
		"invalid decoded transition mesh"
	);
	check(
		!coarse.transitions[face_index].indices.empty(),
		"decoded transition mesh is empty"
	);

	const wt::WtChunkBounds coarse_bounds =
		wt::wt_chunk_bounds(coarse_key);
	const int axis = face_axis(face);
	const double full_plane = static_cast<double>(
		positive_face(face) ?
			axis_coordinate(coarse_bounds.maximum, axis) :
			axis_coordinate(coarse_bounds.minimum, axis)
	);
	const double half_plane = full_plane +
		(positive_face(face) ? -1.0 : 1.0);
	const std::set<Edge> transition_full = plane_boundary_edges(
		coarse.transitions[face_index],
		coarse.world_origin,
		axis,
		full_plane
	);
	const std::set<Edge> transition_half = plane_boundary_edges(
		coarse.transitions[face_index],
		coarse.world_origin,
		axis,
		half_plane
	);
	const std::set<Edge> coarse_regular = plane_boundary_edges(
		coarse.regular,
		coarse.world_origin,
		axis,
		half_plane
	);
	check(
		!transition_full.empty() &&
		transition_half == coarse_regular,
		"decoded transition half contour mismatch"
	);

	std::set<Edge> fine_edges;
	for (const wt::WtChunkKey &key : support_keys) {
		const wt::WtChunkPage *fine_page = find_page(pages, key);
		if (fine_page == nullptr) {
			check(false, "fine support page missing");
			continue;
		}
		const wt::WtChunkPageSampleSource fine_source(*fine_page);
		wt::WtChunkMeshResult fine;
		check(
			mesher.mesh(
				{ key, 0, 0.0F, 0.25F },
				fine_source,
				fine,
				scratch
			) == wt::WtChunkMeshingStatus::Ok,
			"decoded fine regular mesh failed"
		);
		validate_buffer(fine.regular, "invalid decoded fine regular mesh");
		const std::set<Edge> edges = plane_boundary_edges(
			fine.regular,
			fine.world_origin,
			axis,
			full_plane
		);
		fine_edges.insert(edges.begin(), edges.end());
		hash_result(hash, fine);
	}
	check(
		transition_full == fine_edges,
		"decoded page transition does not match fine pages"
	);
	hash_result(hash, coarse);
}

void test_page_lod_corner(
	const wt::WtChunkMesher &mesher,
	wt::WtChunkMeshingScratch &scratch,
	std::uint64_t &hash
) {
	const wt::WtChunkKey coarse_key = { 0, 0, 0, 1 };
	const std::uint8_t mask = static_cast<std::uint8_t>(
		wt::wt_face_bit(wt::WtChunkFace::PositiveX) |
		wt::wt_face_bit(wt::WtChunkFace::PositiveY) |
		wt::wt_face_bit(wt::WtChunkFace::PositiveZ)
	);
	std::vector<wt::WtChunkKey> keys = { coarse_key };
	std::set<wt::WtChunkKey> transition_support_keys;
	for (wt::WtChunkFace face : {
		wt::WtChunkFace::PositiveX,
		wt::WtChunkFace::PositiveY,
		wt::WtChunkFace::PositiveZ,
	}) {
		std::array<wt::WtChunkKey, 4> support{};
		check(
			wt::wt_transition_support_page_keys(
				coarse_key,
				face,
				support
			),
			"corner support key generation failed"
		);
		keys.insert(keys.end(), support.begin(), support.end());
		transition_support_keys.insert(support.begin(), support.end());
	}
	keys.insert(keys.end(), {
		{ 2, 2, 1, 0 },
		{ 2, 1, 2, 0 },
		{ 1, 2, 2, 0 },
		{ 2, 2, 2, 0 },
	});
	SphereSource source;
	source.center = { 32, 32, 32 };
	source.radius = 8.25;
	wt::WtChunkBaker baker(keys.size());
	std::vector<wt::WtBakedChunkPage> baked;
	std::vector<wt::WtChunkPage> pages;
	check(
		baker.bake(keys, 72, source, baked) ==
			wt::WtChunkBakeStatus::Ok &&
		decode_pages(baked, pages),
		"corner page bake/decode failed"
	);
	const wt::WtChunkPage *coarse_page = find_page(pages, coarse_key);
	check(coarse_page != nullptr, "corner coarse page missing");
	if (coarse_page == nullptr) {
		return;
	}
	wt::WtChunkPageSampleSource coarse_source(*coarse_page);
	for (const wt::WtChunkPage &page : pages) {
		if (transition_support_keys.count(page.metadata.key) != 0) {
			check(
				coarse_source.add_transition_support_page(page) ==
					wt::WtChunkPageSampleSourceStatus::Ok,
				"corner support page rejected"
			);
		}
	}
	check(
		coarse_source.has_transition_support(mask),
		"corner transition support incomplete"
	);
	wt::WtChunkMeshResult coarse;
	check(
		mesher.mesh(
			{ coarse_key, mask, 0.0F, 0.25F },
			coarse_source,
			coarse,
			scratch
		) == wt::WtChunkMeshingStatus::Ok,
		"decoded page LOD corner mesh failed"
	);
	wt::WtChunkMeshResult direct_coarse;
	check(
		mesher.mesh(
			{ coarse_key, mask, 0.0F, 0.25F },
			source,
			direct_coarse,
			scratch
		) == wt::WtChunkMeshingStatus::Ok &&
		mesh_hash(coarse) == mesh_hash(direct_coarse),
		"decoded coarse corner differs from direct source"
	);
	EdgeCounts edge_counts;
	add_result_edges(coarse, edge_counts);
	hash_result(hash, coarse);
	for (const wt::WtChunkPage &page : pages) {
		const wt::WtChunkKey &key = page.metadata.key;
		const bool gallery_page =
			key.lod == 0 &&
			key.x >= 1 && key.x <= 2 &&
			key.y >= 1 && key.y <= 2 &&
			key.z >= 1 && key.z <= 2 &&
			!(key.x == 1 && key.y == 1 && key.z == 1);
		if (!gallery_page) {
			continue;
		}
		const wt::WtChunkPageSampleSource fine_source(page);
		wt::WtChunkMeshResult fine;
		check(
			mesher.mesh(
				{ key, 0, 0.0F, 0.25F },
				fine_source,
				fine,
				scratch
			) == wt::WtChunkMeshingStatus::Ok,
			"decoded corner fine mesh failed"
		);
		wt::WtChunkMeshResult direct_fine;
		check(
			mesher.mesh(
				{ key, 0, 0.0F, 0.25F },
				source,
				direct_fine,
				scratch
			) == wt::WtChunkMeshingStatus::Ok &&
			mesh_hash(fine) == mesh_hash(direct_fine),
			"decoded corner fine mesh differs from direct source"
		);
		add_result_edges(fine, edge_counts);
		hash_result(hash, fine);
	}
	check_closed_surface(
		edge_counts,
		"decoded page three-face LOD corner is open"
	);
}

void test_source_failures() {
	LinearSource source;
	const wt::WtChunkKey coarse_key = { 0, 0, 0, 1 };
	std::array<wt::WtChunkKey, 4> support_keys{};
	check(
		wt::wt_transition_support_page_keys(
			coarse_key,
			wt::WtChunkFace::PositiveX,
			support_keys
		),
		"failure fixture support keys failed"
	);
	std::vector<wt::WtChunkKey> keys = {
		coarse_key,
		support_keys[0],
		{ 50, 50, 50, 0 },
	};
	wt::WtChunkBaker baker(keys.size());
	std::vector<wt::WtBakedChunkPage> baked;
	std::vector<wt::WtChunkPage> pages;
	check(
		baker.bake(keys, 81, source, baked) ==
			wt::WtChunkBakeStatus::Ok &&
		decode_pages(baked, pages),
		"failure fixture bake/decode failed"
	);
	const wt::WtChunkPage *coarse = find_page(pages, coarse_key);
	const wt::WtChunkPage *support = find_page(pages, support_keys[0]);
	const wt::WtChunkPage *unrelated =
		find_page(pages, { 50, 50, 50, 0 });
	if (coarse == nullptr || support == nullptr || unrelated == nullptr) {
		check(false, "failure fixture page missing");
		return;
	}
	wt::WtChunkPageSampleSource page_source(*coarse);
	wt::WtChunkPage invalid_primary = *coarse;
	invalid_primary.samples.clear();
	check(
		wt::WtChunkPageSampleSource(invalid_primary).status() ==
			wt::WtChunkPageSampleSourceStatus::InvalidPrimaryPage,
		"invalid primary page was accepted"
	);
	check(
		page_source.add_transition_support_page(*unrelated) ==
			wt::WtChunkPageSampleSourceStatus::InvalidSupportPage,
		"unrelated support page was accepted"
	);
	wt::WtChunkPage wrong_revision = *support;
	++wrong_revision.metadata.source_revision;
	check(
		page_source.add_transition_support_page(wrong_revision) ==
			wt::WtChunkPageSampleSourceStatus::InvalidSupportPage,
		"revision-mismatched support page was accepted"
	);
	check(
		page_source.add_transition_support_page(*support) ==
			wt::WtChunkPageSampleSourceStatus::Ok &&
		page_source.add_transition_support_page(*support) ==
			wt::WtChunkPageSampleSourceStatus::DuplicateSupportPage,
		"duplicate support contract mismatch"
	);
	check(
		!page_source.has_transition_support(
			wt::WtChunkFace::PositiveX
		),
		"partial support set reported complete"
	);

	wt::WtChunkPage conflicting = *support;
	constexpr std::size_t shared_sample_index = (1 * 19 + 1) * 19 + 1;
	conflicting.samples[shared_sample_index].density += 1.0F;
	wt::WtChunkPageSampleSource conflicting_source(*coarse);
	check(
		conflicting_source.add_transition_support_page(conflicting) ==
			wt::WtChunkPageSampleSourceStatus::Ok,
		"conflicting support setup failed"
	);
	const wt::WtChunkBounds support_bounds =
		wt::wt_chunk_bounds(conflicting.metadata.key);
	const wt::WtGridPoint conflict_point = support_bounds.minimum;
	wt::WtScalarSample sample;
	check(
		!conflicting_source.sample(conflict_point, sample),
		"overlapping conflicting sample was accepted"
	);
}

} // namespace

int main() {
	test_support_keys();
	const wt::WtChunkMesher mesher(wt::wt_get_transvoxel_mit_backend());
	wt::WtChunkMeshingScratch scratch;
	std::uint64_t hash = 14695981039346656037ULL;
	for (unsigned int face_index = 0; face_index < 6; ++face_index) {
		run_face_gallery(
			static_cast<wt::WtChunkFace>(face_index),
			mesher,
			scratch,
			hash
		);
	}
	test_page_lod_corner(mesher, scratch, hash);
	test_source_failures();
	constexpr std::uint64_t expected_hash = 0x7717f75423306ccaULL;
	check(hash == expected_hash, "M5 page transition hash mismatch");
	std::printf(
		"M5_PAGE_TRANSITION_HASH %016llx\n",
		static_cast<unsigned long long>(hash)
	);
	if (failure_count != 0) {
		std::fprintf(stderr,
			"M5_PAGE_TRANSITION_FAIL failures=%d\n",
			failure_count
		);
		return 1;
	}
	std::printf(
		"M5_PAGE_TRANSITION_PASS faces=6 support_pages_per_face=4 "
		"coarse_lod=2 corners=1 conflicts=1\n"
	);
	return 0;
}
