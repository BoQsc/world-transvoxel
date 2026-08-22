#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace world_transvoxel {

struct WtCpuWorkBudgetMetrics {
	std::uint64_t capacity = 0;
	std::uint64_t active = 0;
	std::uint64_t waiting = 0;
	std::uint64_t maximum_active = 0;
	std::uint64_t grants = 0;
	std::uint64_t wait_time_ns_last = 0;
	std::uint64_t wait_time_ns_total = 0;
	std::uint64_t wait_time_ns_maximum = 0;
};

class WtCpuWorkBudget {
public:
	class Lease {
	public:
		Lease() noexcept = default;
		Lease(const Lease &) = delete;
		Lease &operator=(const Lease &) = delete;

		Lease(Lease &&other) noexcept : owner_(other.owner_) {
			other.owner_ = nullptr;
		}

		Lease &operator=(Lease &&other) noexcept {
			if (this == &other) return *this;
			reset();
			owner_ = other.owner_;
			other.owner_ = nullptr;
			return *this;
		}

		~Lease() {
			reset();
		}

		explicit operator bool() const noexcept {
			return owner_ != nullptr;
		}

		void reset() noexcept {
			if (owner_ == nullptr) return;
			WtCpuWorkBudget *owner = owner_;
			owner_ = nullptr;
			owner->release();
		}

	private:
		friend class WtCpuWorkBudget;
		explicit Lease(WtCpuWorkBudget *owner) noexcept : owner_(owner) {}

		WtCpuWorkBudget *owner_ = nullptr;
	};

	explicit WtCpuWorkBudget(std::size_t capacity) noexcept :
			capacity_(capacity) {
	}

	WtCpuWorkBudget(const WtCpuWorkBudget &) = delete;
	WtCpuWorkBudget &operator=(const WtCpuWorkBudget &) = delete;

	bool valid() const noexcept {
		return capacity_ != 0;
	}

	std::size_t capacity() const noexcept {
		return capacity_;
	}

	Lease acquire(std::int32_t priority) {
		if (capacity_ == 0) return {};
		const auto started = std::chrono::steady_clock::now();
		Waiter waiter;
		waiter.priority = priority;
		std::unique_lock<std::mutex> lock(mutex_);
		waiter.ticket = next_ticket_++;
		waiters_.push_back(&waiter);
		available_.wait(lock, [&]() {
			return active_ < capacity_ && highest_waiter_locked() == &waiter;
		});
		waiters_.erase(std::find(waiters_.begin(), waiters_.end(), &waiter));
		++active_;
		maximum_active_ = std::max(maximum_active_, active_);
		++grants_;
		const std::uint64_t wait_ns = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - started
			).count()
		);
		wait_time_ns_last_ = wait_ns;
		wait_time_ns_total_ += wait_ns;
		wait_time_ns_maximum_ = std::max(wait_time_ns_maximum_, wait_ns);
		return Lease(this);
	}

	WtCpuWorkBudgetMetrics get_metrics() const noexcept {
		std::lock_guard<std::mutex> lock(mutex_);
		return {
			capacity_,
			active_,
			waiters_.size(),
			maximum_active_,
			grants_,
			wait_time_ns_last_,
			wait_time_ns_total_,
			wait_time_ns_maximum_,
		};
	}

private:
	struct Waiter {
		std::uint64_t ticket = 0;
		std::int32_t priority = 0;
	};

	Waiter *highest_waiter_locked() const noexcept {
		if (waiters_.empty()) return nullptr;
		return *std::max_element(
			waiters_.begin(),
			waiters_.end(),
			[](const Waiter *left, const Waiter *right) {
				if (left->priority != right->priority) {
					return left->priority < right->priority;
				}
				return left->ticket > right->ticket;
			}
		);
	}

	void release() noexcept {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (active_ != 0) --active_;
		}
		available_.notify_all();
	}

	std::size_t capacity_ = 0;
	mutable std::mutex mutex_;
	std::condition_variable available_;
	std::vector<Waiter *> waiters_;
	std::size_t active_ = 0;
	std::size_t maximum_active_ = 0;
	std::uint64_t next_ticket_ = 0;
	std::uint64_t grants_ = 0;
	std::uint64_t wait_time_ns_last_ = 0;
	std::uint64_t wait_time_ns_total_ = 0;
	std::uint64_t wait_time_ns_maximum_ = 0;
};

} // namespace world_transvoxel
