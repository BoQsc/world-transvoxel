#include "storage/wt_world_snapshot_store_io.h"

#include <cstdio>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace world_transvoxel {
namespace {

bool flush_durable(FILE *file) noexcept {
	if (file == nullptr || std::fflush(file) != 0) return false;
#if defined(_WIN32)
	return _commit(_fileno(file)) == 0;
#else
	return fsync(fileno(file)) == 0;
#endif
}

FILE *open_write(const std::filesystem::path &path) {
#if defined(_WIN32)
	return _wfopen(path.c_str(), L"wb");
#else
	return std::fopen(path.c_str(), "wb");
#endif
}

} // namespace

bool wt_snapshot_write_file(
	const std::filesystem::path &path,
	const std::vector<std::uint8_t> &bytes
) {
	FILE *file = open_write(path);
	if (file == nullptr) return false;
	const bool written = bytes.empty() ||
		std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
	const bool durable = written && flush_durable(file);
	const bool closed = std::fclose(file) == 0;
	return durable && closed;
}

bool wt_snapshot_sync_directory(
	const std::filesystem::path &path
) noexcept {
#if defined(_WIN32)
	(void)path;
	return true;
#else
	const int descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY);
	if (descriptor < 0) return false;
	const bool ok = fsync(descriptor) == 0;
	close(descriptor);
	return ok;
#endif
}

std::string wt_snapshot_hash_hex(const WtHash256 &hash) {
	constexpr char digits[] = "0123456789abcdef";
	std::string output(hash.size() * 2, '0');
	for (std::size_t index = 0; index < hash.size(); ++index) {
		output[index * 2] = digits[hash[index] >> 4];
		output[index * 2 + 1] = digits[hash[index] & 0x0fU];
	}
	return output;
}

} // namespace world_transvoxel
