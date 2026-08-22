#include "core/wt_cpu_work_budget.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace wt = world_transvoxel;

namespace {

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

bool wait_for_waiters(
	const wt::WtCpuWorkBudget &budget,
	std::uint64_t expected
) {
	for (std::size_t attempt = 0; attempt < 1000; ++attempt) {
		if (budget.get_metrics().waiting == expected) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

void test_capacity_bound() {
	wt::WtCpuWorkBudget budget(2);
	std::atomic<int> active{ 0 };
	std::atomic<int> maximum{ 0 };
	std::vector<std::thread> workers;
	for (int index = 0; index < 6; ++index) {
		workers.emplace_back([&]() {
			auto lease = budget.acquire(0);
			const int current = active.fetch_add(1) + 1;
			int observed = maximum.load();
			while (current > observed &&
				!maximum.compare_exchange_weak(observed, current)) {
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
			active.fetch_sub(1);
		});
	}
	for (std::thread &worker : workers) worker.join();
	const wt::WtCpuWorkBudgetMetrics metrics = budget.get_metrics();
	check(maximum.load() == 2, "shared work did not use both slots");
	check(metrics.maximum_active == 2,
		"budget maximum active count did not match capacity");
	check(metrics.grants == 6, "budget did not grant every waiting worker");
	check(metrics.active == 0 && metrics.waiting == 0,
		"budget retained active or waiting work after lease release");
}

void test_priority_then_fifo() {
	wt::WtCpuWorkBudget budget(1);
	auto blocker = budget.acquire(0);
	std::mutex order_mutex;
	std::vector<int> order;
	const auto launch = [&](int identity, std::int32_t priority) {
		return std::thread([&, identity, priority]() {
			auto lease = budget.acquire(priority);
			std::lock_guard<std::mutex> lock(order_mutex);
			order.push_back(identity);
		});
	};

	std::thread low = launch(1, -10);
	check(wait_for_waiters(budget, 1), "low-priority waiter did not queue");
	std::thread high_first = launch(2, 100);
	check(wait_for_waiters(budget, 2), "first high-priority waiter did not queue");
	std::thread high_second = launch(3, 100);
	check(wait_for_waiters(budget, 3), "second high-priority waiter did not queue");
	blocker.reset();
	low.join();
	high_first.join();
	high_second.join();

	check(order.size() == 3, "priority test did not complete every waiter");
	check(order.size() == 3 && order[0] == 2 && order[1] == 3 && order[2] == 1,
		"budget did not grant by priority and FIFO ticket order");
}

void test_disabled_budget() {
	wt::WtCpuWorkBudget budget(0);
	auto lease = budget.acquire(100);
	check(!budget.valid() && !lease,
		"zero-capacity budget unexpectedly produced a lease");
}

} // namespace

int main() {
	test_capacity_bound();
	test_priority_then_fifo();
	test_disabled_budget();
	if (failure_count != 0) {
		std::fprintf(stderr, "CPU_WORK_BUDGET_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf(
		"CPU_WORK_BUDGET_PASS capacity=2 grants=6 priority=fifo disabled=ok\n"
	);
	return 0;
}
