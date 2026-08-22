#pragma once

#include "storage/wt_hash256.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace world_transvoxel {

bool wt_snapshot_write_file(
	const std::filesystem::path &path,
	const std::vector<std::uint8_t> &bytes
);

bool wt_snapshot_sync_directory(
	const std::filesystem::path &path
) noexcept;

std::string wt_snapshot_hash_hex(const WtHash256 &hash);

} // namespace world_transvoxel
