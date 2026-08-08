#include "testing/wt_fault_injection.h"

#include <atomic>

namespace world_transvoxel {
namespace {

std::atomic<std::uint8_t> armed_site{
	static_cast<std::uint8_t>(WtFaultInjectionSite::None)
};
std::atomic<std::uint64_t> remaining_matches{ 0 };
std::atomic<std::uint64_t> matching_attempts{ 0 };
std::atomic<std::uint64_t> injected_failures{ 0 };

} // namespace

void wt_arm_fault_injection(
	WtFaultInjectionSite site,
	std::uint64_t fail_on_matching_attempt
) noexcept {
	const bool armed = site != WtFaultInjectionSite::None &&
		fail_on_matching_attempt != 0;
	matching_attempts.store(0, std::memory_order_relaxed);
	injected_failures.store(0, std::memory_order_relaxed);
	remaining_matches.store(
		armed ? fail_on_matching_attempt : 0,
		std::memory_order_release
	);
	armed_site.store(
		static_cast<std::uint8_t>(
			armed ? site : WtFaultInjectionSite::None
		),
		std::memory_order_release
	);
}

void wt_clear_fault_injection() noexcept {
	armed_site.store(
		static_cast<std::uint8_t>(WtFaultInjectionSite::None),
		std::memory_order_release
	);
	remaining_matches.store(0, std::memory_order_release);
}

bool wt_should_inject_fault(WtFaultInjectionSite site) noexcept {
	if (site == WtFaultInjectionSite::None ||
		armed_site.load(std::memory_order_acquire) !=
			static_cast<std::uint8_t>(site)) {
		return false;
	}
	matching_attempts.fetch_add(1, std::memory_order_relaxed);
	std::uint64_t remaining = remaining_matches.load(std::memory_order_acquire);
	while (remaining != 0) {
		if (remaining_matches.compare_exchange_weak(
				remaining,
				remaining - 1,
				std::memory_order_acq_rel,
				std::memory_order_acquire
			)) {
			if (remaining != 1) return false;
			armed_site.store(
				static_cast<std::uint8_t>(WtFaultInjectionSite::None),
				std::memory_order_release
			);
			injected_failures.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
	}
	return false;
}

WtFaultInjectionMetrics wt_fault_injection_metrics() noexcept {
	WtFaultInjectionMetrics output;
	output.site = static_cast<WtFaultInjectionSite>(
		armed_site.load(std::memory_order_acquire)
	);
	output.matching_attempts = matching_attempts.load(std::memory_order_relaxed);
	output.injected_failures = injected_failures.load(std::memory_order_relaxed);
	output.remaining_matches = remaining_matches.load(std::memory_order_relaxed);
	return output;
}

} // namespace world_transvoxel
