#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>

namespace world_transvoxel {

class WorldTransvoxelTerrain : public godot::Node3D {
	GDCLASS(WorldTransvoxelTerrain, godot::Node3D)

protected:
	static void _bind_methods();

public:
	godot::String get_addon_version() const;
	godot::String get_milestone() const;
	bool is_mit_backend_available() const;
	godot::String get_backend_id() const;
	godot::String get_backend_license() const;
	godot::String get_backend_upstream_revision() const;
};

} // namespace world_transvoxel
