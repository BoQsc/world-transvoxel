#include "bake/wt_chunk_baker.h"
#include "storage/wt_hash256.h"
#include "storage/wt_world_manifest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wt = world_transvoxel;

namespace {

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

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

class WorldSource final : public wt::WtChunkSampleSource {
public:
	bool sample(
		const wt::WtGridPoint &point,
		wt::WtScalarSample &output
	) const noexcept override {
		output.density =
			static_cast<float>(point.x + point.z) * 0.125F -
			static_cast<float>(point.y) * 0.25F -
			1.5F;
		output.material = static_cast<std::uint16_t>(
			static_cast<std::uint64_t>(point.x) ^
			(static_cast<std::uint64_t>(point.y) << 1) ^
			(static_cast<std::uint64_t>(point.z) << 2)
		);
		return true;
	}
};

std::vector<wt::WtDependencyEntry> make_dependencies() {
	return {
		dependency(
			wt::WtDependencyKind::Toolchain,
			"zig",
			"0.16.0",
			"zig-0.16.0"
		),
		dependency(
			wt::WtDependencyKind::SourceAsset,
			"terrain/source-a",
			"",
			"source-a-bytes"
		),
		dependency(
			wt::WtDependencyKind::GodotCpp,
			"godot-cpp",
			"e83fd090",
			"godot-cpp-e83fd090"
		),
		dependency(
			wt::WtDependencyKind::Backend,
			"transvoxel-mit",
			"upstream-sha256",
			"transvoxel-upstream"
		),
		dependency(
			wt::WtDependencyKind::Configuration,
			"world-config",
			"1",
			"configuration"
		),
		dependency(
			wt::WtDependencyKind::Generator,
			"world-transvoxel-baker",
			"0.4.0-m3",
			"generator-version"
		),
		dependency(
			wt::WtDependencyKind::Godot,
			"godot",
			"4.6.3",
			"godot-4.6.3"
		),
	};
}

std::vector<wt::WtBakedChunkPage> bake_pages() {
	const WorldSource source;
	const std::vector<wt::WtChunkKey> keys = {
		{ 2, -1, 0, 0 },
		{ -1, 0, 1, 1 },
		{ 0, 0, 0, 0 },
	};
	wt::WtChunkBaker baker(keys.size());
	std::vector<wt::WtBakedChunkPage> pages;
	check(
		baker.bake(keys, 7001, source, pages) == wt::WtChunkBakeStatus::Ok,
		"world fixture bake failed"
	);
	return pages;
}

wt::WtWorldManifest make_manifest(
	const std::vector<wt::WtBakedChunkPage> &pages
) {
	wt::WtWorldManifest manifest;
	manifest.source_revision = 7001;
	manifest.world_revision = 0;
	manifest.configuration_hash = hash_text("configuration");
	manifest.dependencies = make_dependencies();
	for (auto iterator = pages.rbegin(); iterator != pages.rend(); ++iterator) {
		manifest.pages.push_back({
			iterator->key,
			iterator->bytes.size(),
			iterator->content_hash,
		});
	}
	return manifest;
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void test_world_round_trip(
	const std::vector<wt::WtBakedChunkPage> &pages,
	std::vector<std::uint8_t> &world_bytes
) {
	wt::WtWorldManifest first = make_manifest(pages);
	wt::WtWorldManifest second = first;
	std::reverse(second.dependencies.begin(), second.dependencies.end());
	std::reverse(second.pages.begin(), second.pages.end());
	std::vector<std::uint8_t> reordered_bytes;
	check(
		wt::wt_write_world_manifest(first, world_bytes) ==
			wt::WtWorldManifestStatus::Ok,
		"world manifest write failed"
	);
	check(
		wt::wt_write_world_manifest(second, reordered_bytes) ==
			wt::WtWorldManifestStatus::Ok,
		"reordered world manifest write failed"
	);
	check(world_bytes == reordered_bytes, "world manifest is input-order dependent");

	wt::WtWorldManifestView view;
	check(
		wt::wt_open_world_manifest(
			{ world_bytes.data(), world_bytes.size() },
			view
		) == wt::WtWorldManifestStatus::Ok,
		"world manifest open failed"
	);
	check(
		view.source_revision == 7001 &&
			view.world_revision == 0 &&
			view.configuration_hash == hash_text("configuration"),
		"world metadata mismatch"
	);
	check(
		view.dependencies.size() == 7 && view.pages.size() == pages.size(),
		"world manifest counts mismatch"
	);
	check(
		view.container.find_section(wt::kWtWorldMetadataSection) != nullptr &&
			view.container.find_section(wt::kWtWorldDependencySection) != nullptr &&
			view.container.find_section(wt::kWtWorldIndexSection) != nullptr,
		"world sections missing"
	);
	for (const wt::WtBakedChunkPage &page : pages) {
		const wt::WtWorldPageIndexEntry *entry = view.find_page(page.key);
		check(entry != nullptr, "indexed page lookup failed");
		check(
			entry != nullptr &&
				entry->byte_size == page.bytes.size() &&
				entry->content_hash == page.content_hash,
			"indexed page metadata mismatch"
		);
		check(
			wt::wt_validate_world_page(
				view,
				page.key,
				{ page.bytes.data(), page.bytes.size() }
			) == wt::WtWorldPageStatus::Ok,
			"indexed page validation failed"
		);
	}
	check(
		view.find_page({ 99, 99, 99, 0 }) == nullptr &&
			wt::wt_validate_world_page(
				view,
				{ 99, 99, 99, 0 },
				{}
			) == wt::WtWorldPageStatus::PageNotFound,
		"missing page lookup succeeded"
	);

	std::vector<std::uint8_t> short_page = pages[0].bytes;
	short_page.pop_back();
	check(
		wt::wt_validate_world_page(
			view,
			pages[0].key,
			{ short_page.data(), short_page.size() }
		) == wt::WtWorldPageStatus::SizeMismatch,
		"page size mismatch was accepted"
	);
	std::vector<std::uint8_t> corrupt_page = pages[0].bytes;
	corrupt_page.back() ^= 0x40U;
	check(
		wt::wt_validate_world_page(
			view,
			pages[0].key,
			{ corrupt_page.data(), corrupt_page.size() }
		) == wt::WtWorldPageStatus::HashMismatch,
		"page hash mismatch was accepted"
	);
}

void test_manifest_failures(
	const std::vector<wt::WtBakedChunkPage> &pages,
	const std::vector<std::uint8_t> &valid_bytes
) {
	wt::WtWorldManifest manifest = make_manifest(pages);
	std::vector<std::uint8_t> bytes;

	manifest.dependencies.pop_back();
	check(
		wt::wt_write_world_manifest(manifest, bytes) ==
			wt::WtWorldManifestStatus::InvalidInput,
		"incomplete dependency manifest was accepted"
	);
	manifest = make_manifest(pages);
	manifest.dependencies.push_back(manifest.dependencies.front());
	check(
		wt::wt_write_world_manifest(manifest, bytes) ==
			wt::WtWorldManifestStatus::InvalidInput,
		"duplicate dependency identity was accepted"
	);
	manifest = make_manifest(pages);
	manifest.configuration_hash = hash_text("different-config");
	check(
		wt::wt_write_world_manifest(manifest, bytes) ==
			wt::WtWorldManifestStatus::InvalidInput,
		"configuration dependency mismatch was accepted"
	);
	manifest = make_manifest(pages);
	manifest.dependencies[0].label = std::string("bad") + '\x01';
	check(
		wt::wt_write_world_manifest(manifest, bytes) ==
			wt::WtWorldManifestStatus::InvalidInput,
		"control character dependency label was accepted"
	);
	manifest = make_manifest(pages);
	manifest.pages.push_back(manifest.pages.front());
	check(
		wt::wt_write_world_manifest(manifest, bytes) ==
			wt::WtWorldManifestStatus::InvalidInput,
		"duplicate page key was accepted"
	);

	std::vector<std::uint8_t> corrupted = valid_bytes;
	corrupted.back() ^= 0x80U;
	wt::WtWorldManifestView rejected;
	check(
		wt::wt_open_world_manifest(
			{ corrupted.data(), corrupted.size() },
			rejected
		) == wt::WtWorldManifestStatus::ContainerFailure,
		"corrupted world manifest was accepted"
	);

	wt::WtWorldManifestView valid;
	check(
		wt::wt_open_world_manifest(
			{ valid_bytes.data(), valid_bytes.size() },
			valid
		) == wt::WtWorldManifestStatus::Ok,
		"schema fixture world open failed"
	);
	const wt::WtContainerSection *metadata =
		valid.container.find_section(wt::kWtWorldMetadataSection);
	const wt::WtContainerSection *dependencies =
		valid.container.find_section(wt::kWtWorldDependencySection);
	const wt::WtContainerSection *index =
		valid.container.find_section(wt::kWtWorldIndexSection);
	if (metadata == nullptr || dependencies == nullptr || index == nullptr) {
		check(false, "schema fixture sections missing");
		return;
	}
	const std::array<std::uint8_t, 1> extra = { 0 };
	std::vector<wt::WtContainerSectionInput> sections = {
		{ wt::kWtWorldMetadataSection, 1, wt::WtStorageCodec::None,
			metadata->payload },
		{ wt::kWtWorldDependencySection, 0, wt::WtStorageCodec::None,
			dependencies->payload },
		{ wt::kWtWorldIndexSection, 0, wt::WtStorageCodec::None,
			index->payload },
	};
	check(
		wt::wt_write_container(
			wt::kWtWorldMagic,
			0,
			7001,
			sections,
			bytes
		) == wt::WtContainerStatus::Ok &&
			wt::wt_open_world_manifest(
				{ bytes.data(), bytes.size() },
				rejected
			) == wt::WtWorldManifestStatus::InvalidManifest,
		"nonzero world section flags were accepted"
	);
	sections[0].flags = 0;
	sections.push_back({
		wt::wt_fourcc('E', 'X', 'T', 'R'),
		0,
		wt::WtStorageCodec::None,
		{ extra.data(), extra.size() },
	});
	check(
		wt::wt_write_container(
			wt::kWtWorldMagic,
			0,
			7001,
			sections,
			bytes
		) == wt::WtContainerStatus::Ok &&
			wt::wt_open_world_manifest(
				{ bytes.data(), bytes.size() },
				rejected
			) == wt::WtWorldManifestStatus::InvalidManifest,
		"extra world section was accepted"
	);

	manifest = make_manifest(pages);
	manifest.pages[0].key = pages[1].key;
	manifest.pages[0].byte_size = pages[0].bytes.size();
	manifest.pages[0].content_hash = pages[0].content_hash;
	manifest.pages.erase(manifest.pages.begin() + 1);
	check(
		wt::wt_write_world_manifest(manifest, bytes) ==
			wt::WtWorldManifestStatus::Ok &&
			wt::wt_open_world_manifest(
				{ bytes.data(), bytes.size() },
				rejected
			) == wt::WtWorldManifestStatus::Ok &&
			wt::wt_validate_world_page(
				rejected,
				pages[1].key,
				{ pages[0].bytes.data(), pages[0].bytes.size() }
			) == wt::WtWorldPageStatus::MetadataMismatch,
		"page metadata mismatch was accepted"
	);
}

void test_schema_1_0_migration(const std::vector<std::uint8_t> &current_bytes) {
	wt::WtWorldManifestView current;
	check(
		wt::wt_open_world_manifest(
			{ current_bytes.data(), current_bytes.size() },
			current
		) == wt::WtWorldManifestStatus::Ok,
		"schema 1.0 migration fixture failed"
	);
	const wt::WtContainerSection *metadata =
		current.container.find_section(wt::kWtWorldMetadataSection);
	const wt::WtContainerSection *dependencies =
		current.container.find_section(wt::kWtWorldDependencySection);
	const wt::WtContainerSection *index =
		current.container.find_section(wt::kWtWorldIndexSection);
	if (metadata == nullptr || dependencies == nullptr || index == nullptr) {
		check(false, "schema 1.0 migration sections missing");
		return;
	}
	std::vector<std::uint8_t> legacy_metadata(
		metadata->payload.data,
		metadata->payload.data + wt::kWtWorldMetadataV1_0Size
	);
	std::vector<std::uint8_t> legacy_dependencies(
		dependencies->payload.data,
		dependencies->payload.data + dependencies->payload.size
	);
	std::vector<std::uint8_t> legacy_index(
		index->payload.data,
		index->payload.data + index->payload.size
	);
	legacy_metadata[2] = 0;
	legacy_metadata[3] = 0;
	legacy_dependencies[2] = 0;
	legacy_dependencies[3] = 0;
	legacy_index[2] = 0;
	legacy_index[3] = 0;
	const std::vector<wt::WtContainerSectionInput> sections = {
		{ wt::kWtWorldMetadataSection, 0, wt::WtStorageCodec::None,
			{ legacy_metadata.data(), legacy_metadata.size() } },
		{ wt::kWtWorldDependencySection, 0, wt::WtStorageCodec::None,
			{ legacy_dependencies.data(), legacy_dependencies.size() } },
		{ wt::kWtWorldIndexSection, 0, wt::WtStorageCodec::None,
			{ legacy_index.data(), legacy_index.size() } },
	};
	std::vector<std::uint8_t> legacy_bytes;
	wt::WtWorldManifestView legacy;
	check(
		wt::wt_write_container(
			wt::kWtWorldMagic,
			0,
			7001,
			sections,
			legacy_bytes
		) == wt::WtContainerStatus::Ok &&
			wt::wt_open_world_manifest(
				{ legacy_bytes.data(), legacy_bytes.size() },
				legacy
			) == wt::WtWorldManifestStatus::Ok &&
			legacy.source_revision == 7001 &&
			legacy.world_revision == 0 &&
			legacy.pages == current.pages &&
			legacy.dependencies.size() == current.dependencies.size(),
		"schema 1.0 world migration failed"
	);
}

} // namespace

int main() {
	const std::vector<wt::WtBakedChunkPage> pages = bake_pages();
	if (pages.size() != 3) {
		std::fprintf(stderr, "M4_WORLD_FAIL fixture_pages=%zu\n", pages.size());
		return 1;
	}
	std::vector<std::uint8_t> world_bytes;
	test_world_round_trip(pages, world_bytes);
	test_manifest_failures(pages, world_bytes);
	test_schema_1_0_migration(world_bytes);
	if (failure_count != 0) {
		std::fprintf(stderr, "M4_WORLD_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("M4_WORLD_HASH ");
	print_hash(wt::wt_sha256(world_bytes.data(), world_bytes.size()));
	std::printf(
		"M4_WORLD_PASS pages=3 dependencies=7 failure_cases=13\n"
	);
	return 0;
}
