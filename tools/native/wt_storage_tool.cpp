#include "editing/wt_edit_journal.h"
#include "storage/wt_binary_io.h"
#include "storage/wt_chunk_page.h"
#include "storage/wt_world_manifest.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace wt = world_transvoxel;

namespace {

constexpr std::uint64_t kMaximumToolFileSize = 1024ULL * 1024ULL * 1024ULL;

bool read_file(
	const std::filesystem::path &path,
	std::vector<std::uint8_t> &output
) {
	output.clear();
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input) return false;
	const std::streamoff size = input.tellg();
	if (size < 0 ||
		static_cast<std::uint64_t>(size) > kMaximumToolFileSize) {
		return false;
	}
	output.resize(static_cast<std::size_t>(size));
	input.seekg(0);
	if (!output.empty()) {
		input.read(
			reinterpret_cast<char *>(output.data()),
			static_cast<std::streamsize>(output.size())
		);
	}
	return input.good() || input.eof();
}

bool write_new_file(
	const std::filesystem::path &path,
	const std::vector<std::uint8_t> &bytes
) {
	std::error_code error;
	if (std::filesystem::exists(path, error) || error) return false;
	const std::filesystem::path temporary =
		path.parent_path() / (path.filename().string() + ".tmp");
	if (std::filesystem::exists(temporary, error) || error) return false;
	{
		std::ofstream output(temporary, std::ios::binary);
		if (!output) return false;
		if (!bytes.empty()) {
			output.write(
				reinterpret_cast<const char *>(bytes.data()),
				static_cast<std::streamsize>(bytes.size())
			);
		}
		if (!output) {
			output.close();
			std::filesystem::remove(temporary, error);
			return false;
		}
	}
	std::filesystem::rename(temporary, path, error);
	if (error) {
		std::filesystem::remove(temporary, error);
		return false;
	}
	return true;
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
}

bool section_schema(
	const wt::WtContainerView &container,
	std::uint32_t type,
	std::uint16_t &major,
	std::uint16_t &minor
) {
	const wt::WtContainerSection *section = container.find_section(type);
	if (section == nullptr) return false;
	wt::WtBinaryReader reader(section->payload);
	return reader.read_u16(major) == wt::WtBinaryStatus::Ok &&
		reader.read_u16(minor) == wt::WtBinaryStatus::Ok;
}

int inspect_world(const std::vector<std::uint8_t> &bytes) {
	wt::WtWorldManifestView world;
	if (wt::wt_open_world_manifest(
			{ bytes.data(), bytes.size() },
			world
		) != wt::WtWorldManifestStatus::Ok) {
		return 2;
	}
	std::uint16_t major = 0;
	std::uint16_t minor = 0;
	if (!section_schema(
			world.container,
			wt::kWtWorldMetadataSection,
			major,
			minor
		)) {
		return 2;
	}
	std::printf(
		"{\"type\":\"wtworld\",\"schema_major\":%u,"
		"\"schema_minor\":%u,\"source_revision\":%llu,"
		"\"world_revision\":%llu,\"pages\":%zu,\"dependencies\":%zu,"
		"\"sha256\":\"",
		static_cast<unsigned int>(major),
		static_cast<unsigned int>(minor),
		static_cast<unsigned long long>(world.source_revision),
		static_cast<unsigned long long>(world.world_revision),
		world.pages.size(),
		world.dependencies.size()
	);
	print_hash(wt::wt_sha256(bytes.data(), bytes.size()));
	std::printf("\"}\n");
	return 0;
}

int inspect_chunk(const std::vector<std::uint8_t> &bytes) {
	wt::WtChunkPageView page;
	if (wt::wt_open_chunk_page(
			{ bytes.data(), bytes.size() },
			page
		) != wt::WtChunkPageStatus::Ok) {
		return 2;
	}
	std::printf(
		"{\"type\":\"wtchunk\",\"schema_major\":%u,"
		"\"schema_minor\":%u,\"source_revision\":%llu,"
		"\"key\":[%d,%d,%d,%u],\"samples\":%u,\"sha256\":\"",
		static_cast<unsigned int>(wt::kWtChunkPageSchemaMajor),
		static_cast<unsigned int>(wt::kWtChunkPageSchemaMinor),
		static_cast<unsigned long long>(page.metadata.source_revision),
		page.metadata.key.x,
		page.metadata.key.y,
		page.metadata.key.z,
		static_cast<unsigned int>(page.metadata.key.lod),
		page.metadata.sample_count
	);
	print_hash(wt::wt_sha256(bytes.data(), bytes.size()));
	std::printf("\"}\n");
	return 0;
}

int inspect_journal(const std::vector<std::uint8_t> &bytes) {
	if (bytes.empty()) return 2;
	std::size_t offset = 0;
	std::size_t transaction_count = 0;
	std::size_t command_count = 0;
	std::uint64_t source_revision = 0;
	std::uint64_t initial_revision = 0;
	while (offset < bytes.size()) {
		std::size_t segment_size = 0;
		if (wt::wt_measure_container(
				{ bytes.data() + offset, bytes.size() - offset },
				wt::kWtEditMagic,
				segment_size
			) != wt::WtContainerStatus::Ok ||
			segment_size == 0) {
			return 2;
		}
		wt::WtEditTransactionDocument transaction;
		if (wt::wt_open_edit_transaction(
				{ bytes.data() + offset, segment_size },
				transaction
			) != wt::WtEditTransactionStatus::Ok) {
			return 2;
		}
		if (transaction_count == 0) {
			source_revision = transaction.transaction.source_revision;
			initial_revision = transaction.transaction.base_revision;
		}
		command_count += transaction.transaction.commands.size();
		++transaction_count;
		offset += segment_size;
	}
	wt::WtEditJournal journal(transaction_count, command_count, bytes.size());
	std::size_t committed_bytes = 0;
	if (journal.load(
			{ bytes.data(), bytes.size() },
			source_revision,
			initial_revision,
			false,
			committed_bytes
		) != wt::WtEditJournalStatus::Ok ||
		committed_bytes != bytes.size()) {
		return 2;
	}
	std::printf(
		"{\"type\":\"wtedit\",\"source_revision\":%llu,"
		"\"initial_world_revision\":%llu,\"world_revision\":%llu,"
		"\"transactions\":%zu,\"commands\":%zu,\"sha256\":\"",
		static_cast<unsigned long long>(journal.source_revision()),
		static_cast<unsigned long long>(journal.initial_world_revision()),
		static_cast<unsigned long long>(journal.current_world_revision()),
		journal.transaction_count(),
		journal.command_count()
	);
	print_hash(wt::wt_sha256(bytes.data(), bytes.size()));
	std::printf("\"}\n");
	return 0;
}

int inspect_file(const std::filesystem::path &path) {
	std::vector<std::uint8_t> bytes;
	if (!read_file(path, bytes) || bytes.size() < 8) return 2;
	wt::WtFormatMagic magic{};
	std::copy(bytes.begin(), bytes.begin() + 8, magic.begin());
	if (magic == wt::kWtWorldMagic) return inspect_world(bytes);
	if (magic == wt::kWtChunkMagic) return inspect_chunk(bytes);
	if (magic == wt::kWtEditMagic) return inspect_journal(bytes);
	return 2;
}

int migrate_world(
	const std::filesystem::path &input_path,
	const std::filesystem::path &output_path
) {
	std::vector<std::uint8_t> input;
	if (!read_file(input_path, input)) return 2;
	wt::WtWorldManifestView view;
	if (wt::wt_open_world_manifest(
			{ input.data(), input.size() },
			view
		) != wt::WtWorldManifestStatus::Ok) {
		return 2;
	}
	wt::WtWorldManifest manifest;
	manifest.source_revision = view.source_revision;
	manifest.world_revision = view.world_revision;
	manifest.configuration_hash = view.configuration_hash;
	manifest.dependencies = view.dependencies;
	manifest.pages = view.pages;
	std::vector<std::uint8_t> output;
	if (wt::wt_write_world_manifest(manifest, output) !=
		wt::WtWorldManifestStatus::Ok ||
		!write_new_file(output_path, output)) {
		return 2;
	}
	return inspect_world(output);
}

void print_usage() {
	std::fprintf(
		stderr,
		"usage: wt_storage_tool inspect <path> | validate <path> | "
		"migrate-world <input> <output>\n"
	);
}

} // namespace

int main(int argc, char **argv) {
	if (argc == 3 &&
		(std::string(argv[1]) == "inspect" ||
			std::string(argv[1]) == "validate")) {
		return inspect_file(argv[2]);
	}
	if (argc == 4 && std::string(argv[1]) == "migrate-world") {
		return migrate_world(argv[2], argv[3]);
	}
	print_usage();
	return 1;
}
