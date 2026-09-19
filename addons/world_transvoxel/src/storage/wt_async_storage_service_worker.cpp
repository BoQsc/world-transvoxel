#include "storage/wt_async_storage_service.h"

#include "storage/wt_chunk_page.h"

#include <algorithm>

namespace world_transvoxel {

void WtAsyncStorageService::start_workers(
	std::size_t worker_count,
	WtStorageRequestClass request_class
) {
	std::lock_guard<std::mutex> lock(mutex_);
	workers_.reserve(workers_.size() + worker_count);
	for (std::size_t index = 0; index < worker_count; ++index) {
		workers_.emplace_back(
			&WtAsyncStorageService::worker_main,
			this,
			request_class
		);
	}
}

void WtAsyncStorageService::worker_main(
	WtStorageRequestClass request_class
) noexcept {
	for (;;) {
		Request request;
		std::chrono::steady_clock::time_point load_started;
		WtAsyncStorageTraceObserver trace_observer;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			const auto eligible = [&]() {
				return std::find_if(
					requests_.begin(), requests_.end(),
					[&](const Request &candidate) {
						return limits_.interaction_request_capacity == 0 ||
							candidate.request_class == request_class;
					}
				);
			};
			work_available_.wait(lock, [&]() {
				return stop_requested_ || eligible() != requests_.end();
			});
			if (stop_requested_) return;
			const auto selected = eligible();
			request = *selected;
			requests_.erase(selected);
			++metrics_.started_requests;
			metrics_.interaction_started_requests +=
				request.request_class == WtStorageRequestClass::Interaction ? 1U : 0U;
			load_started = std::chrono::steady_clock::now();
			in_flight_requests_.push_back({
				{ request.key, request.generation },
				load_started,
				request.request_class,
			});
			metrics_.maximum_in_flight_requests = std::max<std::uint64_t>(
				metrics_.maximum_in_flight_requests,
				in_flight_requests_.size()
			);
			trace_observer = trace_observer_;
		}
		if (trace_observer) {
			trace_observer(WtAsyncStorageTraceEventKind::Started, request.key,
				request.generation, 0, WtPageLoadStatus::Ok);
		}
		const auto traced_load_started = std::chrono::steady_clock::now();
		std::uint64_t bytes_read = 0;
		WtPageLoadCompletion completion = load_page(request, bytes_read);
		if (completion.status == WtPageLoadStatus::Ok &&
			completion.page_bytes) {
			WtChunkPageView view;
			auto decoded_page = std::make_shared<WtChunkPage>();
			if (wt_open_chunk_page(
					{ completion.page_bytes->data(), completion.page_bytes->size() },
					view
				) != WtChunkPageStatus::Ok ||
				view.metadata.key != completion.key ||
				wt_decode_chunk_page(view, *decoded_page) !=
					WtChunkPageStatus::Ok) {
				completion.status = WtPageLoadStatus::PageFailure;
				completion.page_bytes.reset();
			} else {
				completion.decoded_page = std::move(decoded_page);
			}
		}
		const auto load_finished = std::chrono::steady_clock::now();
		const auto elapsed_ns = [](auto duration) {
			return static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()
			);
		};
		const std::uint64_t observed_load_time_ns =
			elapsed_ns(load_finished - load_started);
		if (trace_observer) {
			trace_observer(WtAsyncStorageTraceEventKind::Finished, request.key,
				request.generation, elapsed_ns(load_finished - traced_load_started),
				completion.status);
		}

		std::unique_lock<std::mutex> lock(mutex_);
		metrics_.load_time_ns_last = observed_load_time_ns;
		metrics_.load_time_ns_total += observed_load_time_ns;
		metrics_.load_time_ns_maximum = std::max(
			metrics_.load_time_ns_maximum, observed_load_time_ns
		);
		const auto in_flight = std::find_if(
			in_flight_requests_.begin(), in_flight_requests_.end(),
			[&](const InFlightRequest &candidate) {
				return candidate.identity.key == request.key;
			}
		);
		if (in_flight != in_flight_requests_.end()) {
			in_flight_requests_.erase(in_flight);
		}
		completion_space_available_.wait(lock, [&]() {
			return stop_requested_ || completion_count_ < limits_.completion_capacity;
		});
		if (stop_requested_) {
			++metrics_.cancelled_requests;
			return;
		}
		const std::size_t tail = (completion_head_ + completion_count_) %
			limits_.completion_capacity;
		completion_slots_[tail] = std::move(completion);
		++completion_count_;
		++metrics_.completed_requests;
		metrics_.bytes_read += bytes_read;
		if (completion_slots_[tail].status == WtPageLoadStatus::Ok) {
			++metrics_.successful_pages;
		} else {
			++metrics_.failed_pages;
		}
		completion_available_.notify_one();
		if (completion_notifier_) completion_notifier_();
	}
}

} // namespace world_transvoxel
