#pragma once

#include "core/wt_chunk_key.h"
#include "storage/wt_container_format.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

constexpr std::uint16_t kWtEditSchemaMajor = 1;
constexpr std::uint16_t kWtEditSchemaMinor = 0;
constexpr std::size_t kWtEditHeaderSize = 64;
constexpr std::size_t kWtEditCommitSize = 68;
constexpr std::size_t kWtEditCommandSectionHeaderSize = 8;
constexpr std::size_t kWtEditCommandHeaderSize = 108;
constexpr std::size_t kWtEditRecordHashSize = 32;
constexpr std::size_t kWtMaximumEditCommandCount = 4096;
constexpr std::int64_t kWtEditCoordinateScale = 65536;
constexpr std::uint32_t kWtEditHeaderSection =
	wt_fourcc('E', 'H', 'D', 'R');
constexpr std::uint32_t kWtEditCommandSection =
	wt_fourcc('C', 'M', 'D', 'S');
constexpr std::uint32_t kWtEditCommitSection =
	wt_fourcc('C', 'M', 'I', 'T');

using WtId128 = std::array<std::uint8_t, 16>;

enum class WtEditOperation : std::uint16_t {
	AddDensity = 1,
	SetDensity = 2,
	PaintMaterial = 3,
};

enum class WtEditShape : std::uint16_t {
	Sphere = 1,
	AxisAlignedBox = 2,
};

struct WtEditBounds {
	WtGridPoint minimum;
	WtGridPoint maximum;

	bool operator==(const WtEditBounds &other) const noexcept;
};

struct WtEditSphere {
	std::int64_t center_x_q16 = 0;
	std::int64_t center_y_q16 = 0;
	std::int64_t center_z_q16 = 0;
	std::uint64_t radius_q16 = 0;
};

struct WtEditBox {
	std::int64_t minimum_x_q16 = 0;
	std::int64_t minimum_y_q16 = 0;
	std::int64_t minimum_z_q16 = 0;
	std::int64_t maximum_x_q16 = 0;
	std::int64_t maximum_y_q16 = 0;
	std::int64_t maximum_z_q16 = 0;
};

struct WtEditCommand {
	WtId128 command_id{};
	std::uint32_t sequence = 0;
	std::uint64_t world_revision = 0;
	WtEditOperation operation = WtEditOperation::AddDensity;
	WtEditShape shape = WtEditShape::Sphere;
	std::uint32_t flags = 0;
	float density_value = 0.0F;
	std::uint16_t material = 0;
	WtEditBounds bounds;
	WtEditSphere sphere;
	WtEditBox box;
};

struct WtEditTransaction {
	std::uint64_t source_revision = 0;
	WtId128 transaction_id{};
	std::uint64_t base_revision = 0;
	std::uint64_t committed_revision = 0;
	std::uint64_t author_id = 0;
	std::vector<WtEditCommand> commands;
};

struct WtEditTransactionDocument {
	WtContainerView container;
	WtEditTransaction transaction;
};

enum class WtEditTransactionStatus : std::uint8_t {
	Ok,
	InvalidInput,
	InvalidTransaction,
	CapacityExceeded,
	ContainerFailure,
	HashMismatch,
};

bool wt_is_zero_id(const WtId128 &id) noexcept;

bool wt_edit_sphere_bounds(
	const WtEditSphere &sphere,
	WtEditBounds &output
) noexcept;

bool wt_edit_box_bounds(
	const WtEditBox &box,
	WtEditBounds &output
) noexcept;

bool wt_is_valid_edit_command(const WtEditCommand &command) noexcept;

WtEditTransactionStatus wt_write_edit_transaction(
	const WtEditTransaction &transaction,
	std::vector<std::uint8_t> &output
);

WtEditTransactionStatus wt_open_edit_transaction(
	WtByteView bytes,
	WtEditTransactionDocument &output
);

} // namespace world_transvoxel
