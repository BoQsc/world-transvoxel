#include "api/world_transvoxel_edit_transaction.h"

#include <godot_cpp/core/class_db.hpp>

#include <cmath>
#include <limits>

namespace world_transvoxel {
namespace {

void write_u64(WtId128 &id, std::size_t offset, std::uint64_t value) {
	for (std::size_t index = 0; index < 8; ++index) {
		id[offset + index] =
			static_cast<std::uint8_t>(value >> (index * 8U));
	}
}

WtId128 transaction_id(
	std::uint64_t source_revision,
	std::uint64_t committed_revision
) {
	WtId128 id{};
	write_u64(id, 0, source_revision ^ 0x5754545849443031ULL);
	write_u64(id, 8, committed_revision ^ 0x4544495454524e31ULL);
	return id;
}

WtId128 command_id(
	std::uint64_t committed_revision,
	std::uint32_t sequence
) {
	WtId128 id{};
	write_u64(id, 0, committed_revision ^ 0x5754434d44494431ULL);
	id[8] = static_cast<std::uint8_t>(sequence);
	id[9] = static_cast<std::uint8_t>(sequence >> 8U);
	id[10] = static_cast<std::uint8_t>(sequence >> 16U);
	id[11] = static_cast<std::uint8_t>(sequence >> 24U);
	id[12] = 'C';
	id[13] = 'M';
	id[14] = 'D';
	id[15] = '1';
	return id;
}

bool to_q16(double value, std::int64_t &output) noexcept {
	if (!std::isfinite(value)) return false;
	const long double scaled =
		static_cast<long double>(value) * kWtEditCoordinateScale;
	if (scaled < std::numeric_limits<std::int64_t>::min() ||
		scaled > std::numeric_limits<std::int64_t>::max()) {
		return false;
	}
	output = static_cast<std::int64_t>(std::llround(scaled));
	return true;
}

bool valid_density(double value) noexcept {
	return std::isfinite(value) &&
		value >= -std::numeric_limits<float>::max() &&
		value <= std::numeric_limits<float>::max();
}

} // namespace

void WorldTransvoxelEditTransaction::_bind_methods() {
#define WT_BIND_EDIT_METHOD(name, ...) \
	godot::ClassDB::bind_method( \
		godot::D_METHOD(#name, __VA_ARGS__), \
		&WorldTransvoxelEditTransaction::name \
	)
	WT_BIND_EDIT_METHOD(add_density_sphere, "center", "radius", "value");
	WT_BIND_EDIT_METHOD(set_density_sphere, "center", "radius", "value");
	WT_BIND_EDIT_METHOD(carve_sdf_sphere, "center", "radius", "strength");
	WT_BIND_EDIT_METHOD(
		carve_smooth_sdf_sphere,
		"center",
		"radius",
		"strength",
		"smooth_radius"
	);
	WT_BIND_EDIT_METHOD(construct_sdf_sphere, "center", "radius", "strength");
	WT_BIND_EDIT_METHOD(
		construct_smooth_sdf_sphere,
		"center",
		"radius",
		"strength",
		"smooth_radius"
	);
	WT_BIND_EDIT_METHOD(
		construct_material_sdf_sphere,
		"center",
		"radius",
		"strength",
		"material"
	);
	WT_BIND_EDIT_METHOD(
		construct_material_smooth_sdf_sphere,
		"center",
		"radius",
		"strength",
		"material",
		"smooth_radius"
	);
	WT_BIND_EDIT_METHOD(paint_material_sphere, "center", "radius", "material");
	WT_BIND_EDIT_METHOD(place_material_volume_sphere, "center", "radius", "material");
	WT_BIND_EDIT_METHOD(add_density_box, "minimum", "maximum", "value");
	WT_BIND_EDIT_METHOD(set_density_box, "minimum", "maximum", "value");
	WT_BIND_EDIT_METHOD(paint_material_box, "minimum", "maximum", "material");
	WT_BIND_EDIT_METHOD(place_material_volume_box, "minimum", "maximum", "material");
#undef WT_BIND_EDIT_METHOD
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_base_revision"),
		&WorldTransvoxelEditTransaction::get_base_revision
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_committed_revision"),
		&WorldTransvoxelEditTransaction::get_committed_revision
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_command_count"),
		&WorldTransvoxelEditTransaction::get_command_count
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("is_submitted"),
		&WorldTransvoxelEditTransaction::is_submitted
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_error"),
		&WorldTransvoxelEditTransaction::get_error
	);
}

void WorldTransvoxelEditTransaction::initialize(
	std::uint64_t source_revision,
	std::uint64_t base_revision,
	std::uint64_t author_id
) {
	transaction_ = {};
	transaction_.source_revision = source_revision;
	transaction_.base_revision = base_revision;
	transaction_.committed_revision = base_revision + 1;
	transaction_.author_id = author_id;
	transaction_.transaction_id = transaction_id(
		source_revision, transaction_.committed_revision
	);
	initialized_ = base_revision != std::numeric_limits<std::uint64_t>::max();
	submitted_ = false;
	error_ = initialized_ ? "ok" : "world revision is exhausted";
}

bool WorldTransvoxelEditTransaction::append_command(WtEditCommand command) {
	if (!initialized_ || submitted_) {
		error_ = submitted_ ? "transaction is already submitted" :
			"transaction is not initialized";
		return false;
	}
	if (transaction_.commands.size() >= kWtMaximumEditCommandCount) {
		error_ = "transaction command capacity is exceeded";
		return false;
	}
	command.sequence = static_cast<std::uint32_t>(
		transaction_.commands.size()
	);
	command.world_revision = transaction_.committed_revision;
	command.command_id = command_id(
		transaction_.committed_revision, command.sequence
	);
	if (!wt_is_valid_edit_command(command)) {
		error_ = "edit command is invalid";
		return false;
	}
	transaction_.commands.push_back(command);
	error_ = "ok";
	return true;
}

bool WorldTransvoxelEditTransaction::append_sphere(
	WtEditOperation operation,
	const godot::Vector3 &center,
	double radius,
	double density,
	std::int64_t material,
	double smooth_radius
) {
	WtEditCommand command;
	command.operation = operation;
	command.shape = WtEditShape::Sphere;
	command.density_value = static_cast<float>(density);
	command.material = static_cast<std::uint16_t>(material);
	std::int64_t radius_q16 = 0;
	std::int64_t smooth_radius_q16 = 0;
	if (!valid_density(density) || radius <= 0.0 ||
		!to_q16(center.x, command.sphere.center_x_q16) ||
		!to_q16(center.y, command.sphere.center_y_q16) ||
		!to_q16(center.z, command.sphere.center_z_q16) ||
		!to_q16(radius, radius_q16) || radius_q16 <= 0 ||
		!to_q16(smooth_radius, smooth_radius_q16) ||
		smooth_radius_q16 < 0 ||
		smooth_radius_q16 > std::numeric_limits<std::uint32_t>::max() ||
		material < 0 || material > std::numeric_limits<std::uint16_t>::max()) {
		error_ = "sphere edit parameters are invalid";
		return false;
	}
	command.sphere.radius_q16 = static_cast<std::uint64_t>(radius_q16);
	command.smooth_radius_q16 = static_cast<std::uint32_t>(smooth_radius_q16);
	if (!wt_edit_sphere_bounds(
			command.sphere, command.bounds, command.smooth_radius_q16
		)) {
		error_ = "sphere edit bounds are invalid";
		return false;
	}
	return append_command(command);
}

bool WorldTransvoxelEditTransaction::append_box(
	WtEditOperation operation,
	const godot::Vector3 &minimum,
	const godot::Vector3 &maximum,
	double density,
	std::int64_t material
) {
	WtEditCommand command;
	command.operation = operation;
	command.shape = WtEditShape::AxisAlignedBox;
	command.density_value = static_cast<float>(density);
	command.material = static_cast<std::uint16_t>(material);
	if (!valid_density(density) ||
		!to_q16(minimum.x, command.box.minimum_x_q16) ||
		!to_q16(minimum.y, command.box.minimum_y_q16) ||
		!to_q16(minimum.z, command.box.minimum_z_q16) ||
		!to_q16(maximum.x, command.box.maximum_x_q16) ||
		!to_q16(maximum.y, command.box.maximum_y_q16) ||
		!to_q16(maximum.z, command.box.maximum_z_q16) ||
		material < 0 || material > std::numeric_limits<std::uint16_t>::max() ||
		!wt_edit_box_bounds(command.box, command.bounds)) {
		error_ = "box edit parameters are invalid";
		return false;
	}
	return append_command(command);
}

bool WorldTransvoxelEditTransaction::add_density_sphere(
	const godot::Vector3 &center, double radius, double value
) {
	return append_sphere(
		WtEditOperation::AddDensity, center, radius, value, 0
	);
}

bool WorldTransvoxelEditTransaction::set_density_sphere(
	const godot::Vector3 &center, double radius, double value
) {
	return append_sphere(
		WtEditOperation::SetDensity, center, radius, value, 0
	);
}

bool WorldTransvoxelEditTransaction::carve_sdf_sphere(
	const godot::Vector3 &center, double radius, double strength
) {
	return append_sphere(
		WtEditOperation::SdfCarve, center, radius, strength, 0
	);
}

bool WorldTransvoxelEditTransaction::carve_smooth_sdf_sphere(
	const godot::Vector3 &center,
	double radius,
	double strength,
	double smooth_radius
) {
	return append_sphere(
		WtEditOperation::SdfCarve,
		center,
		radius,
		strength,
		0,
		smooth_radius
	);
}

bool WorldTransvoxelEditTransaction::construct_sdf_sphere(
	const godot::Vector3 &center, double radius, double strength
) {
	return append_sphere(
		WtEditOperation::SdfConstruct, center, radius, strength, 0
	);
}

bool WorldTransvoxelEditTransaction::construct_smooth_sdf_sphere(
	const godot::Vector3 &center,
	double radius,
	double strength,
	double smooth_radius
) {
	return append_sphere(
		WtEditOperation::SdfConstruct,
		center,
		radius,
		strength,
		0,
		smooth_radius
	);
}

bool WorldTransvoxelEditTransaction::construct_material_sdf_sphere(
	const godot::Vector3 &center,
	double radius,
	double strength,
	std::int64_t material
) {
	if (material <= 0) {
		error_ = "construct material must be greater than zero";
		return false;
	}
	return append_sphere(
		WtEditOperation::SdfConstruct, center, radius, strength, material
	);
}

bool WorldTransvoxelEditTransaction::construct_material_smooth_sdf_sphere(
	const godot::Vector3 &center,
	double radius,
	double strength,
	std::int64_t material,
	double smooth_radius
) {
	if (material <= 0) {
		error_ = "construct material must be greater than zero";
		return false;
	}
	return append_sphere(
		WtEditOperation::SdfConstruct,
		center,
		radius,
		strength,
		material,
		smooth_radius
	);
}

bool WorldTransvoxelEditTransaction::paint_material_sphere(
	const godot::Vector3 &center, double radius, std::int64_t material
) {
	return append_sphere(
		WtEditOperation::PaintMaterial, center, radius, 0.0, material
	);
}

bool WorldTransvoxelEditTransaction::place_material_volume_sphere(
	const godot::Vector3 &center, double radius, std::int64_t material
) {
	if (material <= 0) {
		error_ = "volume material must be greater than zero";
		return false;
	}
	return append_sphere(
		WtEditOperation::PlaceMaterialVolume, center, radius, 0.0, material
	);
}

bool WorldTransvoxelEditTransaction::add_density_box(
	const godot::Vector3 &minimum,
	const godot::Vector3 &maximum,
	double value
) {
	return append_box(
		WtEditOperation::AddDensity, minimum, maximum, value, 0
	);
}

bool WorldTransvoxelEditTransaction::set_density_box(
	const godot::Vector3 &minimum,
	const godot::Vector3 &maximum,
	double value
) {
	return append_box(
		WtEditOperation::SetDensity, minimum, maximum, value, 0
	);
}

bool WorldTransvoxelEditTransaction::paint_material_box(
	const godot::Vector3 &minimum,
	const godot::Vector3 &maximum,
	std::int64_t material
) {
	return append_box(
		WtEditOperation::PaintMaterial, minimum, maximum, 0.0, material
	);
}

bool WorldTransvoxelEditTransaction::place_material_volume_box(
	const godot::Vector3 &minimum,
	const godot::Vector3 &maximum,
	std::int64_t material
) {
	if (material <= 0) {
		error_ = "volume material must be greater than zero";
		return false;
	}
	return append_box(
		WtEditOperation::PlaceMaterialVolume,
		minimum,
		maximum,
		0.0,
		material
	);
}

std::int64_t
WorldTransvoxelEditTransaction::get_base_revision() const noexcept {
	return static_cast<std::int64_t>(transaction_.base_revision);
}

std::int64_t
WorldTransvoxelEditTransaction::get_committed_revision() const noexcept {
	return static_cast<std::int64_t>(transaction_.committed_revision);
}

std::int64_t
WorldTransvoxelEditTransaction::get_command_count() const noexcept {
	return static_cast<std::int64_t>(transaction_.commands.size());
}

bool WorldTransvoxelEditTransaction::is_submitted() const noexcept {
	return submitted_;
}

godot::String WorldTransvoxelEditTransaction::get_error() const {
	return error_;
}

} // namespace world_transvoxel
