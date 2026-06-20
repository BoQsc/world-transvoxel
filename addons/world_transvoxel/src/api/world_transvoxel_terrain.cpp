#include "api/world_transvoxel_terrain.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "core/wt_version.h"

#include <godot_cpp/core/class_db.hpp>

namespace world_transvoxel {

void WorldTransvoxelTerrain::_bind_methods() {
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_addon_version"),
		&WorldTransvoxelTerrain::get_addon_version
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_milestone"),
		&WorldTransvoxelTerrain::get_milestone
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("is_mit_backend_available"),
		&WorldTransvoxelTerrain::is_mit_backend_available
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_backend_id"),
		&WorldTransvoxelTerrain::get_backend_id
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_backend_license"),
		&WorldTransvoxelTerrain::get_backend_license
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("get_backend_upstream_revision"),
		&WorldTransvoxelTerrain::get_backend_upstream_revision
	);
}

godot::String WorldTransvoxelTerrain::get_addon_version() const {
	return kAddonVersion;
}

godot::String WorldTransvoxelTerrain::get_milestone() const {
	return kMilestone;
}

bool WorldTransvoxelTerrain::is_mit_backend_available() const {
	return wt_get_transvoxel_mit_backend().is_available();
}

godot::String WorldTransvoxelTerrain::get_backend_id() const {
	return wt_get_transvoxel_mit_backend().get_info().id;
}

godot::String WorldTransvoxelTerrain::get_backend_license() const {
	return wt_get_transvoxel_mit_backend().get_info().license;
}

godot::String WorldTransvoxelTerrain::get_backend_upstream_revision() const {
	return wt_get_transvoxel_mit_backend().get_info().upstream_revision;
}

} // namespace world_transvoxel
