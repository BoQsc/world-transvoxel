#include "register_types.h"

#include "api/world_transvoxel_chunk_state.h"
#include "api/world_transvoxel_config.h"
#include "api/world_transvoxel_edit_transaction.h"
#include "api/world_transvoxel_sample.h"
#include "api/world_transvoxel_terrain.h"

#include <gdextension_interface.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

void initialize_world_transvoxel_module(
	godot::ModuleInitializationLevel initialization_level
) {
	if (initialization_level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(world_transvoxel::WorldTransvoxelChunkState);
	GDREGISTER_CLASS(world_transvoxel::WorldTransvoxelConfig);
	GDREGISTER_CLASS(world_transvoxel::WorldTransvoxelEditTransaction);
	GDREGISTER_CLASS(world_transvoxel::WorldTransvoxelSample);
	GDREGISTER_CLASS(world_transvoxel::WorldTransvoxelTerrain);
}

void uninitialize_world_transvoxel_module(
	godot::ModuleInitializationLevel initialization_level
) {
	if (initialization_level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

extern "C" {

GDExtensionBool GDE_EXPORT world_transvoxel_library_init(
	GDExtensionInterfaceGetProcAddress get_proc_address,
	GDExtensionClassLibraryPtr library,
	GDExtensionInitialization *initialization
) {
	godot::GDExtensionBinding::InitObject init_object(
		get_proc_address,
		library,
		initialization
	);

	init_object.register_initializer(initialize_world_transvoxel_module);
	init_object.register_terminator(uninitialize_world_transvoxel_module);
	init_object.set_minimum_library_initialization_level(
		godot::MODULE_INITIALIZATION_LEVEL_SCENE
	);

	return init_object.init();
}

}
