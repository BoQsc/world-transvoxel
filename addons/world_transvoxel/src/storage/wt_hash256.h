#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace world_transvoxel {

using WtHash256 = std::array<std::uint8_t, 32>;

WtHash256 wt_sha256(const std::uint8_t *data, std::size_t size) noexcept;

inline bool wt_is_zero_hash(const WtHash256 &hash) noexcept {
	for (std::uint8_t byte : hash) {
		if (byte != 0) {
			return false;
		}
	}
	return true;
}

} // namespace world_transvoxel
