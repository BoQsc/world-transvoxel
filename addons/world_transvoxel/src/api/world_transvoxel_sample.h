#pragma once

#include "services/wt_authoritative_sample_query.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <cstdint>

namespace world_transvoxel {

class WorldTransvoxelTerrain;

class WorldTransvoxelSample : public godot::RefCounted {
	GDCLASS(WorldTransvoxelSample, godot::RefCounted)

protected:
	static void _bind_methods();

public:
	godot::Vector3i get_grid_point() const noexcept;
	std::int64_t get_lod() const noexcept;
	double get_density() const noexcept;
	std::int64_t get_material() const noexcept;
	std::int64_t get_source_revision() const noexcept;
	std::int64_t get_world_revision() const noexcept;
	std::int64_t get_agreeing_page_count() const noexcept;

private:
	friend class WorldTransvoxelTerrain;

	void set_sample(const WtAuthoritativeSample &sample) noexcept;

	WtAuthoritativeSample sample_;
};

} // namespace world_transvoxel
