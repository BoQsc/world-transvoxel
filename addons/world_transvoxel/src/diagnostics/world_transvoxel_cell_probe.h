#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <cstdint>

namespace world_transvoxel {

class WorldTransvoxelCellProbe : public godot::RefCounted {
	GDCLASS(WorldTransvoxelCellProbe, godot::RefCounted)

protected:
	static void _bind_methods();

public:
	godot::Dictionary get_backend_identity() const;
	godot::Dictionary get_gpu_meshing_tables() const;
	godot::Dictionary mesh_regular_cell(
		const godot::PackedFloat32Array &densities,
		const godot::PackedVector3Array &gradients,
		const godot::PackedInt32Array &materials,
		const godot::Vector3 &origin,
		double cell_size,
		double isovalue
	) const;
	godot::Dictionary mesh_transition_cell(
		const godot::PackedFloat32Array &densities,
		const godot::PackedVector3Array &gradients,
		const godot::PackedInt32Array &materials,
		std::int64_t orientation,
		const godot::Vector3 &full_resolution_origin,
		double sample_spacing,
		double transition_width,
		double isovalue
	) const;
	godot::Dictionary mesh_chunk_with_callable(
		const godot::Callable &sample_callable,
		const godot::Vector3i &chunk_coordinate,
		std::int64_t lod,
		std::int64_t transition_mask,
		std::int64_t cached_transition_mask,
		double isovalue,
		double transition_width_ratio
	) const;
};

} // namespace world_transvoxel
