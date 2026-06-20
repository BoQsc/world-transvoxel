#include "storage/wt_binary_io.h"

#include <cstring>
#include <limits>
#include <utility>

namespace world_transvoxel {

static_assert(sizeof(float) == sizeof(std::uint32_t));
static_assert(std::numeric_limits<float>::is_iec559);

WtBinaryWriter::WtBinaryWriter(std::size_t capacity) : capacity_(capacity) {
	bytes_.reserve(capacity);
}

WtBinaryStatus WtBinaryWriter::write_u8(std::uint8_t value) {
	return write_bytes(&value, 1);
}

WtBinaryStatus WtBinaryWriter::write_u16(std::uint16_t value) {
	const std::uint8_t bytes[2] = {
		static_cast<std::uint8_t>(value),
		static_cast<std::uint8_t>(value >> 8),
	};
	return write_bytes(bytes, sizeof(bytes));
}

WtBinaryStatus WtBinaryWriter::write_u32(std::uint32_t value) {
	const std::uint8_t bytes[4] = {
		static_cast<std::uint8_t>(value),
		static_cast<std::uint8_t>(value >> 8),
		static_cast<std::uint8_t>(value >> 16),
		static_cast<std::uint8_t>(value >> 24),
	};
	return write_bytes(bytes, sizeof(bytes));
}

WtBinaryStatus WtBinaryWriter::write_u64(std::uint64_t value) {
	std::uint8_t bytes[8]{};
	for (unsigned int index = 0; index < 8; ++index) {
		bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
	}
	return write_bytes(bytes, sizeof(bytes));
}

WtBinaryStatus WtBinaryWriter::write_i32(std::int32_t value) {
	return write_u32(static_cast<std::uint32_t>(value));
}

WtBinaryStatus WtBinaryWriter::write_i64(std::int64_t value) {
	return write_u64(static_cast<std::uint64_t>(value));
}

WtBinaryStatus WtBinaryWriter::write_f32(float value) {
	std::uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	return write_u32(bits);
}

WtBinaryStatus WtBinaryWriter::write_bytes(
	const std::uint8_t *data,
	std::size_t size
) {
	if (size != 0 && data == nullptr) {
		return WtBinaryStatus::InvalidInput;
	}
	if (size == 0) {
		return WtBinaryStatus::Ok;
	}
	if (!can_append(size)) {
		return WtBinaryStatus::CapacityExceeded;
	}
	bytes_.insert(bytes_.end(), data, data + size);
	return WtBinaryStatus::Ok;
}

WtBinaryStatus WtBinaryWriter::patch_bytes(
	std::size_t offset,
	const std::uint8_t *data,
	std::size_t size
) {
	if ((size != 0 && data == nullptr) || offset > bytes_.size() ||
		size > bytes_.size() - offset) {
		return WtBinaryStatus::OutOfBounds;
	}
	if (size != 0) {
		std::memcpy(bytes_.data() + offset, data, size);
	}
	return WtBinaryStatus::Ok;
}

const std::vector<std::uint8_t> &WtBinaryWriter::bytes() const noexcept {
	return bytes_;
}

std::vector<std::uint8_t> WtBinaryWriter::take_bytes() {
	return std::move(bytes_);
}

bool WtBinaryWriter::can_append(std::size_t size) const noexcept {
	return bytes_.size() <= capacity_ && size <= capacity_ - bytes_.size();
}

WtBinaryReader::WtBinaryReader(WtByteView bytes) noexcept : bytes_(bytes) {
}

WtBinaryStatus WtBinaryReader::read_u8(std::uint8_t &value) {
	WtByteView bytes;
	const WtBinaryStatus status = read_bytes(1, bytes);
	if (status == WtBinaryStatus::Ok) {
		value = bytes.data[0];
	}
	return status;
}

WtBinaryStatus WtBinaryReader::read_u16(std::uint16_t &value) {
	WtByteView bytes;
	const WtBinaryStatus status = read_bytes(2, bytes);
	if (status == WtBinaryStatus::Ok) {
		value =
			static_cast<std::uint16_t>(bytes.data[0]) |
			(static_cast<std::uint16_t>(bytes.data[1]) << 8);
	}
	return status;
}

WtBinaryStatus WtBinaryReader::read_u32(std::uint32_t &value) {
	WtByteView bytes;
	const WtBinaryStatus status = read_bytes(4, bytes);
	if (status == WtBinaryStatus::Ok) {
		value = 0;
		for (unsigned int index = 0; index < 4; ++index) {
			value |= static_cast<std::uint32_t>(bytes.data[index]) << (index * 8);
		}
	}
	return status;
}

WtBinaryStatus WtBinaryReader::read_u64(std::uint64_t &value) {
	WtByteView bytes;
	const WtBinaryStatus status = read_bytes(8, bytes);
	if (status == WtBinaryStatus::Ok) {
		value = 0;
		for (unsigned int index = 0; index < 8; ++index) {
			value |= static_cast<std::uint64_t>(bytes.data[index]) << (index * 8);
		}
	}
	return status;
}

WtBinaryStatus WtBinaryReader::read_i32(std::int32_t &value) {
	std::uint32_t unsigned_value = 0;
	const WtBinaryStatus status = read_u32(unsigned_value);
	if (status == WtBinaryStatus::Ok) {
		std::memcpy(&value, &unsigned_value, sizeof(value));
	}
	return status;
}

WtBinaryStatus WtBinaryReader::read_i64(std::int64_t &value) {
	std::uint64_t unsigned_value = 0;
	const WtBinaryStatus status = read_u64(unsigned_value);
	if (status == WtBinaryStatus::Ok) {
		std::memcpy(&value, &unsigned_value, sizeof(value));
	}
	return status;
}

WtBinaryStatus WtBinaryReader::read_f32(float &value) {
	std::uint32_t bits = 0;
	const WtBinaryStatus status = read_u32(bits);
	if (status == WtBinaryStatus::Ok) {
		std::memcpy(&value, &bits, sizeof(value));
	}
	return status;
}

WtBinaryStatus WtBinaryReader::read_bytes(std::size_t size, WtByteView &value) {
	if (!can_read(size)) {
		value = {};
		return WtBinaryStatus::OutOfBounds;
	}
	value = {
		size == 0 ? bytes_.data : bytes_.data + position_,
		size,
	};
	position_ += size;
	return WtBinaryStatus::Ok;
}

WtBinaryStatus WtBinaryReader::seek(std::size_t offset) noexcept {
	if (offset > bytes_.size) {
		return WtBinaryStatus::OutOfBounds;
	}
	position_ = offset;
	return WtBinaryStatus::Ok;
}

std::size_t WtBinaryReader::position() const noexcept {
	return position_;
}

std::size_t WtBinaryReader::remaining() const noexcept {
	return bytes_.size - position_;
}

bool WtBinaryReader::can_read(std::size_t size) const noexcept {
	return position_ <= bytes_.size && size <= bytes_.size - position_;
}

} // namespace world_transvoxel
