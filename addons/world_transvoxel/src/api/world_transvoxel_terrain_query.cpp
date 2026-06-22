#include "api/world_transvoxel_terrain.h"

#include "services/wt_chunk_application.h"

namespace world_transvoxel {

godot::Ref<WorldTransvoxelChunkState>
WorldTransvoxelTerrain::query_chunk_state(
	const godot::Vector3i &chunk_coordinate,
	std::int64_t lod
) const {
	if (lod < 0 || lod > kWtMaximumLod) return {};
	const WtChunkKey key {
		chunk_coordinate.x,
		chunk_coordinate.y,
		chunk_coordinate.z,
		static_cast<std::uint8_t>(lod),
	};
	godot::Ref<WorldTransvoxelChunkState> snapshot;
	snapshot.instantiate();
	snapshot->set_snapshot(key, application_->find_record(key));
	return snapshot;
}

} // namespace world_transvoxel
