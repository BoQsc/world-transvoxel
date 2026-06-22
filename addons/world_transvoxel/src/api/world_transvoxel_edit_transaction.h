#pragma once

#include "editing/wt_edit_transaction.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace world_transvoxel {

class WorldTransvoxelTerrain;

class WorldTransvoxelEditTransaction : public godot::RefCounted {
	GDCLASS(WorldTransvoxelEditTransaction, godot::RefCounted)

protected:
	static void _bind_methods();

public:
	bool add_density_sphere(
		const godot::Vector3 &center,
		double radius,
		double value
	);
	bool set_density_sphere(
		const godot::Vector3 &center,
		double radius,
		double value
	);
	bool paint_material_sphere(
		const godot::Vector3 &center,
		double radius,
		std::int64_t material
	);
	bool add_density_box(
		const godot::Vector3 &minimum,
		const godot::Vector3 &maximum,
		double value
	);
	bool set_density_box(
		const godot::Vector3 &minimum,
		const godot::Vector3 &maximum,
		double value
	);
	bool paint_material_box(
		const godot::Vector3 &minimum,
		const godot::Vector3 &maximum,
		std::int64_t material
	);

	std::int64_t get_base_revision() const noexcept;
	std::int64_t get_committed_revision() const noexcept;
	std::int64_t get_command_count() const noexcept;
	bool is_submitted() const noexcept;
	godot::String get_error() const;

private:
	friend class WorldTransvoxelTerrain;

	void initialize(
		std::uint64_t source_revision,
		std::uint64_t base_revision,
		std::uint64_t author_id
	);
	bool append_sphere(
		WtEditOperation operation,
		const godot::Vector3 &center,
		double radius,
		double density,
		std::int64_t material
	);
	bool append_box(
		WtEditOperation operation,
		const godot::Vector3 &minimum,
		const godot::Vector3 &maximum,
		double density,
		std::int64_t material
	);
	bool append_command(WtEditCommand command);

	WtEditTransaction transaction_;
	bool initialized_ = false;
	bool submitted_ = false;
	godot::String error_ = "transaction is not initialized";
};

} // namespace world_transvoxel
