#include "api/world_transvoxel_sample.h"

#include <godot_cpp/core/class_db.hpp>

namespace world_transvoxel {

void WorldTransvoxelSample::_bind_methods() {
#define WT_BIND_SAMPLE_GETTER(name) \
	godot::ClassDB::bind_method( \
		godot::D_METHOD(#name), \
		&WorldTransvoxelSample::name \
	)
	WT_BIND_SAMPLE_GETTER(get_grid_point);
	WT_BIND_SAMPLE_GETTER(get_lod);
	WT_BIND_SAMPLE_GETTER(get_density);
	WT_BIND_SAMPLE_GETTER(get_material);
	WT_BIND_SAMPLE_GETTER(get_source_revision);
	WT_BIND_SAMPLE_GETTER(get_world_revision);
	WT_BIND_SAMPLE_GETTER(get_agreeing_page_count);
#undef WT_BIND_SAMPLE_GETTER
}

godot::Vector3i WorldTransvoxelSample::get_grid_point() const noexcept {
	return {
		static_cast<std::int32_t>(sample_.point.x),
		static_cast<std::int32_t>(sample_.point.y),
		static_cast<std::int32_t>(sample_.point.z),
	};
}

std::int64_t WorldTransvoxelSample::get_lod() const noexcept {
	return sample_.lod;
}

double WorldTransvoxelSample::get_density() const noexcept {
	return sample_.sample.density;
}

std::int64_t WorldTransvoxelSample::get_material() const noexcept {
	return sample_.sample.material;
}

std::int64_t WorldTransvoxelSample::get_source_revision() const noexcept {
	return static_cast<std::int64_t>(sample_.source_revision);
}

std::int64_t WorldTransvoxelSample::get_world_revision() const noexcept {
	return static_cast<std::int64_t>(sample_.world_revision);
}

std::int64_t
WorldTransvoxelSample::get_agreeing_page_count() const noexcept {
	return static_cast<std::int64_t>(sample_.agreeing_page_count);
}

void WorldTransvoxelSample::set_sample(
	const WtAuthoritativeSample &sample
) noexcept {
	sample_ = sample;
}

} // namespace world_transvoxel
