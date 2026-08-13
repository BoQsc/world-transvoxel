#include "services/wt_world_lifecycle.h"

namespace world_transvoxel {

WtReadOnlyRuntimeStatus
WtWorldLifecycleService::request_visibility_coverage_priority_batch(
	const std::vector<WtVisibilityCoveragePriorityRequest> &requests
) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (state_ != WtWorldLifecycleState::Running || !runtime_) {
		return WtReadOnlyRuntimeStatus::NotRunning;
	}
	return runtime_->request_visibility_coverage_priority_batch(requests);
}

} // namespace world_transvoxel
