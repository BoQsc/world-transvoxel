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

WtReadOnlyRuntimeStatus
WtWorldLifecycleService::update_foreground_priority_lease(
	const WtForegroundPriorityLeaseRequest &request
) {
	std::lock_guard<std::mutex> lock(state_mutex_);
	if (state_ != WtWorldLifecycleState::Running || !runtime_) {
		return WtReadOnlyRuntimeStatus::NotRunning;
	}
	return runtime_->update_foreground_priority_lease(request);
}

} // namespace world_transvoxel
