#pragma once

#include <godot_cpp/core/class_db.hpp>

void initialize_world_transvoxel_module(
	godot::ModuleInitializationLevel initialization_level
);
void uninitialize_world_transvoxel_module(
	godot::ModuleInitializationLevel initialization_level
);
