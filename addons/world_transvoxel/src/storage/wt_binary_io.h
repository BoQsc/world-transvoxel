#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

enum class WtBinaryStatus : std::uint8_t {
	Ok,
	InvalidInput,
	OutOfBounds,
	CapacityExceeded,
};

struct WtByteView {
	const std::uint8_t *data = nullptr;
	std::size_t size = 0;

	bool empty() const noexcept {
		return size == 0;
	}
};

class WtBinaryWriter {
public:
	explicit WtBinaryWriter(std::size_t capacity);

	WtBinaryStatus write_u8(std::uint8_t value);
	WtBinaryStatus write_u16(std::uint16_t value);
	WtBinaryStatus write_u32(std::uint32_t value);
	WtBinaryStatus write_u64(std::uint64_t value);
	WtBinaryStatus write_i32(std::int32_t value);
	WtBinaryStatus write_i64(std::int64_t value);
	WtBinaryStatus write_f32(float value);
	WtBinaryStatus write_bytes(const std::uint8_t *data, std::size_t size);
	WtBinaryStatus patch_bytes(
		std::size_t offset,
		const std::uint8_t *data,
		std::size_t size
	);

	const std::vector<std::uint8_t> &bytes() const noexcept;
	std::vector<std::uint8_t> take_bytes();

private:
	bool can_append(std::size_t size) const noexcept;

	std::size_t capacity_ = 0;
	std::vector<std::uint8_t> bytes_;
};

class WtBinaryReader {
public:
	explicit WtBinaryReader(WtByteView bytes) noexcept;

	WtBinaryStatus read_u8(std::uint8_t &value);
	WtBinaryStatus read_u16(std::uint16_t &value);
	WtBinaryStatus read_u32(std::uint32_t &value);
	WtBinaryStatus read_u64(std::uint64_t &value);
	WtBinaryStatus read_i32(std::int32_t &value);
	WtBinaryStatus read_i64(std::int64_t &value);
	WtBinaryStatus read_f32(float &value);
	WtBinaryStatus read_bytes(std::size_t size, WtByteView &value);
	WtBinaryStatus seek(std::size_t offset) noexcept;

	std::size_t position() const noexcept;
	std::size_t remaining() const noexcept;

private:
	bool can_read(std::size_t size) const noexcept;

	WtByteView bytes_;
	std::size_t position_ = 0;
};

} // namespace world_transvoxel
