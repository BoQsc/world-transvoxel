#include "services/wt_world_lifecycle.h"

namespace world_transvoxel {

WtReadOnlyRuntimeStatus
WtWorldLifecycleService::request_visibility_coverage_priority(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (state_ != WtWorldLifecycleState::Running || !runtime_) {
		return WtReadOnlyRuntimeStatus::NotRunning;
	}
	return runtime_->request_visibility_coverage_priority(key, generation);
}

} // namespace world_transvoxel
