#pragma once

#include "services/wt_page_meshing_runtime.h"

namespace world_transvoxel {

struct WtPageMeshingRuntimeService::PreparedDependency {
	WtChunkKey key;
	std::shared_ptr<const WtChunkPage> page;
};

struct WtPageMeshingRuntimeService::PreparedMeshJob {
	WtChunkJob job;
	std::uint8_t transition_mask = 0;
	std::uint8_t cached_transition_mask = 0;
	bool visual_required = true;
	std::vector<PreparedDependency> dependencies;
	WtTerrainMeshReadyCallback terrain_mesh_ready;
	WtMeshExecutionCallback execution_callback;
	WtMeshCellCaptureCallback cell_capture_callback;
	bool pre_mesh_field_capture = false;
	std::uint64_t enqueued_time_ns = 0;
};

struct WtPageMeshingRuntimeService::PreparedMeshCompletion {
	PreparedMeshJob prepared;
	std::shared_ptr<WtChunkMeshResult> mesh;
	std::shared_ptr<WtChunkMeshResult> water_mesh;
	std::vector<WtRecordedMeshingCell> terrain_records;
	std::vector<WtRecordedMeshingCell> water_records;
	WtPageMeshingRuntimeStatus status =
		WtPageMeshingRuntimeStatus::MeshingFailure;
	std::uint64_t queue_wait_ns = 0;
	std::uint64_t execute_time_ns = 0;
};

} // namespace world_transvoxel
