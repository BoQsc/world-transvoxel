#pragma once

#include <cstdint>

namespace world_transvoxel {

enum class WtFaultInjectionSite : std::uint8_t {
	None = 0,
	PageBufferAllocation = 1,
	SnapshotWorkspaceAllocation = 2,
};

struct WtFaultInjectionMetrics {
	WtFaultInjectionSite site = WtFaultInjectionSite::None;
	std::uint64_t matching_attempts = 0;
	std::uint64_t injected_failures = 0;
	std::uint64_t remaining_matches = 0;
};

void wt_arm_fault_injection(
	WtFaultInjectionSite site,
	std::uint64_t fail_on_matching_attempt
) noexcept;
void wt_clear_fault_injection() noexcept;
bool wt_should_inject_fault(WtFaultInjectionSite site) noexcept;
WtFaultInjectionMetrics wt_fault_injection_metrics() noexcept;

} // namespace world_transvoxel
