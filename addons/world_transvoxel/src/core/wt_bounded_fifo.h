#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

namespace world_transvoxel {

template <typename Value>
class WtBoundedFifo {
public:
	explicit WtBoundedFifo(std::size_t capacity) : slots_(capacity) {
	}

	bool push(const Value &value) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (count_ >= slots_.size() || slots_.empty()) {
			return false;
		}
		const std::size_t tail = (head_ + count_) % slots_.size();
		slots_[tail] = value;
		++count_;
		return true;
	}

	bool pop(Value &value) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (count_ == 0) {
			return false;
		}
		value = slots_[head_];
		head_ = (head_ + 1) % slots_.size();
		--count_;
		return true;
	}

	std::size_t size() const noexcept {
		std::lock_guard<std::mutex> lock(mutex_);
		return count_;
	}

	std::size_t capacity() const noexcept {
		return slots_.size();
	}

private:
	mutable std::mutex mutex_;
	std::vector<Value> slots_;
	std::size_t head_ = 0;
	std::size_t count_ = 0;
};

} // namespace world_transvoxel
