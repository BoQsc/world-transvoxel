#include "api/world_transvoxel_terrain.h"

#include <godot_cpp/core/class_db.hpp>

namespace world_transvoxel {

void WorldTransvoxelTerrain::bind_metrics_methods() {
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_runtime_metrics"),
		&WorldTransvoxelTerrain::get_runtime_metrics
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("begin_cpu_causal_trace"),
		&WorldTransvoxelTerrain::begin_cpu_causal_trace
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("end_cpu_causal_trace"),
		&WorldTransvoxelTerrain::end_cpu_causal_trace
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"get_cpu_causal_trace_events",
			"first_sequence",
			"maximum_events"
		),
		&WorldTransvoxelTerrain::get_cpu_causal_trace_events
	);
}

} // namespace world_transvoxel
