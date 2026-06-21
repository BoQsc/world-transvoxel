#pragma once

#include <cstdint>
#include <filesystem>

namespace world_transvoxel::testing {

bool wt_write_production_world_fixture(
	const std::filesystem::path &root,
	std::uint64_t source_revision,
	std::uint64_t world_revision,
	std::filesystem::path &world_manifest_path
);

} // namespace world_transvoxel::testing
