#include "editing/wt_edit_transaction.h"

#include <cmath>
#include <limits>

namespace world_transvoxel {
namespace {

std::int64_t floor_q16(std::int64_t value) noexcept {
	std::int64_t quotient = value / kWtEditCoordinateScale;
	if (value % kWtEditCoordinateScale < 0) {
		--quotient;
	}
	return quotient;
}

std::int64_t ceil_q16(std::int64_t value) noexcept {
	std::int64_t quotient = value / kWtEditCoordinateScale;
	if (value % kWtEditCoordinateScale > 0) {
		++quotient;
	}
	return quotient;
}

bool subtract_radius(
	std::int64_t center,
	std::uint64_t radius,
	std::int64_t &output
) noexcept {
	if (radius > static_cast<std::uint64_t>(
			std::numeric_limits<std::int64_t>::max()
		)) {
		return false;
	}
	const std::int64_t signed_radius = static_cast<std::int64_t>(radius);
	if (center < std::numeric_limits<std::int64_t>::min() + signed_radius) {
		return false;
	}
	output = center - signed_radius;
	return true;
}

bool add_radius(
	std::int64_t center,
	std::uint64_t radius,
	std::int64_t &output
) noexcept {
	if (radius > static_cast<std::uint64_t>(
			std::numeric_limits<std::int64_t>::max()
		)) {
		return false;
	}
	const std::int64_t signed_radius = static_cast<std::int64_t>(radius);
	if (center > std::numeric_limits<std::int64_t>::max() - signed_radius) {
		return false;
	}
	output = center + signed_radius;
	return true;
}

bool valid_operation(WtEditOperation operation) noexcept {
	return operation == WtEditOperation::AddDensity ||
		operation == WtEditOperation::SetDensity ||
		operation == WtEditOperation::PaintMaterial ||
		operation == WtEditOperation::SdfCarve ||
		operation == WtEditOperation::SdfConstruct ||
		operation == WtEditOperation::PlaceMaterialVolume;
}

bool valid_shape(WtEditShape shape) noexcept {
	return shape == WtEditShape::Sphere ||
		shape == WtEditShape::AxisAlignedBox;
}

bool density_operation(WtEditOperation operation) noexcept {
	return operation == WtEditOperation::AddDensity ||
		operation == WtEditOperation::SetDensity ||
		operation == WtEditOperation::SdfCarve ||
		operation == WtEditOperation::SdfConstruct;
}

bool sdf_operation(WtEditOperation operation) noexcept {
	return operation == WtEditOperation::SdfCarve ||
		operation == WtEditOperation::SdfConstruct;
}

} // namespace

bool WtEditBounds::operator==(const WtEditBounds &other) const noexcept {
	return minimum == other.minimum && maximum == other.maximum;
}

bool wt_is_zero_id(const WtId128 &id) noexcept {
	for (std::uint8_t byte : id) {
		if (byte != 0) return false;
	}
	return true;
}

bool wt_edit_sphere_bounds(
	const WtEditSphere &sphere,
	WtEditBounds &output,
	std::uint32_t smooth_radius_q16
) noexcept {
	if (sphere.radius_q16 == 0) return false;
	if (sphere.radius_q16 > std::numeric_limits<std::uint64_t>::max() -
			smooth_radius_q16) {
		return false;
	}
	const std::uint64_t affected_radius =
		sphere.radius_q16 + smooth_radius_q16;
	std::int64_t minimum[3]{};
	std::int64_t maximum[3]{};
	const std::int64_t centers[3] = {
		sphere.center_x_q16,
		sphere.center_y_q16,
		sphere.center_z_q16,
	};
	for (std::size_t axis = 0; axis < 3; ++axis) {
		if (!subtract_radius(centers[axis], affected_radius, minimum[axis]) ||
			!add_radius(centers[axis], affected_radius, maximum[axis])) {
			return false;
		}
	}
	output = {
		{ floor_q16(minimum[0]), floor_q16(minimum[1]), floor_q16(minimum[2]) },
		{ ceil_q16(maximum[0]), ceil_q16(maximum[1]), ceil_q16(maximum[2]) },
	};
	return true;
}

bool wt_edit_box_bounds(
	const WtEditBox &box,
	WtEditBounds &output
) noexcept {
	if (box.minimum_x_q16 > box.maximum_x_q16 ||
		box.minimum_y_q16 > box.maximum_y_q16 ||
		box.minimum_z_q16 > box.maximum_z_q16) {
		return false;
	}
	output = {
		{
			floor_q16(box.minimum_x_q16),
			floor_q16(box.minimum_y_q16),
			floor_q16(box.minimum_z_q16),
		},
		{
			ceil_q16(box.maximum_x_q16),
			ceil_q16(box.maximum_y_q16),
			ceil_q16(box.maximum_z_q16),
		},
	};
	return true;
}

bool wt_is_valid_edit_command(const WtEditCommand &command) noexcept {
	if (wt_is_zero_id(command.command_id) ||
		!valid_operation(command.operation) ||
		!valid_shape(command.shape) ||
		command.flags != 0 ||
		!std::isfinite(command.density_value)) {
		return false;
	}
	if (density_operation(command.operation) && command.material != 0 &&
		command.operation != WtEditOperation::SdfConstruct) {
		return false;
	}
	if (command.operation == WtEditOperation::AddDensity &&
		command.density_value == 0.0F) {
		return false;
	}
	if (sdf_operation(command.operation) &&
		(command.shape != WtEditShape::Sphere ||
			command.density_value <= 0.0F)) {
		return false;
	}
	if (!sdf_operation(command.operation) && command.smooth_radius_q16 != 0) {
		return false;
	}
	if ((command.operation == WtEditOperation::PaintMaterial ||
			command.operation == WtEditOperation::PlaceMaterialVolume) &&
		command.density_value != 0.0F) {
		return false;
	}
	if (command.operation == WtEditOperation::PlaceMaterialVolume &&
		command.material == 0) {
		return false;
	}
	WtEditBounds expected;
	const bool valid_bounds = command.shape == WtEditShape::Sphere ?
		wt_edit_sphere_bounds(
			command.sphere, expected, command.smooth_radius_q16
		) :
		wt_edit_box_bounds(command.box, expected);
	return valid_bounds && command.bounds == expected;
}

} // namespace world_transvoxel
