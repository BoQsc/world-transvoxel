#pragma once

#include "editing/wt_edit_spatial_index.h"
#include "services/wt_chunk_application.h"
#include "services/wt_chunk_resource_cache.h"
#include "services/wt_desired_set_runtime.h"
#include "services/wt_edit_runtime_replacement.h"
#include "storage/wt_storage_page_cache.h"
#include "streaming/wt_multi_viewer_desired_set.h"
#include "streaming/wt_stream_scheduler.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel::testing {

struct WtM5WorkloadMetrics {
	std::uint64_t frames = 0;
	std::uint64_t viewer_events = 0;
	std::uint64_t edit_events = 0;
	std::uint64_t worker_jobs = 0;
	std::size_t maximum_desired_chunks = 0;
	std::size_t maximum_scheduler_records = 0;
	std::size_t maximum_job_queue = 0;
	std::size_t maximum_render_queue = 0;
	std::size_t maximum_collision_queue = 0;
	std::size_t maximum_resource_entries = 0;
	std::uint64_t maximum_readiness_latency_frames = 0;
};

class WtM5WorkloadFixture {
public:
	WtM5WorkloadFixture();

	bool valid() const noexcept;
	bool update_viewer(
		const WtViewerSnapshot &snapshot,
		const std::vector<WtViewerChunkDemand> &demands
	);
	bool remove_viewer(std::uint64_t viewer_id, std::uint64_t revision);
	bool apply_edit(const WtGridPoint &center);
	bool run_frame(
		std::size_t worker_budget,
		std::size_t render_budget,
		std::size_t collision_budget
	);
	bool drain(std::size_t maximum_frames);

	const WtMultiViewerDesiredSet &desired_set() const noexcept;
	const WtStreamScheduler &scheduler() const noexcept;
	const WtChunkApplicationService &application() const noexcept;
	const WtChunkResourceCache &resource_cache() const noexcept;
	const WtDesiredSetRuntimeService &runtime_service() const noexcept;
	const WtEditRuntimeReplacementService &edit_service() const noexcept;
	WtM5WorkloadMetrics get_metrics() const noexcept;
	std::uint64_t world_revision() const noexcept;

private:
	struct PendingReadiness {
		WtChunkKey key;
		WtGenerationToken generation;
		std::uint64_t start_frame = 0;
	};

	class RenderSink final : public WtRenderSink {
	public:
		bool apply_render(const WtRenderPayload &) override;
	};

	class CollisionSink final : public WtCollisionSink {
	public:
		bool apply_collision(const WtCollisionPayload &) override;
	};

	bool apply_runtime_delta(const WtDesiredSetDelta &delta);
	bool publish_ready_chunk(
		const WtChunkKey &key,
		WtGenerationToken generation
	);
	void mark_pending(
		const WtChunkKey &key,
		WtGenerationToken generation
	);
	void remove_pending(const WtChunkKey &key);
	void resolve_pending();
	void update_maxima();

	static constexpr std::uint64_t kSourceRevision = 700;
	static constexpr std::size_t kChunkCapacity = 32;
	static constexpr std::size_t kQueueCapacity = 128;

	std::uint64_t world_revision_ = 7;
	WtMultiViewerDesiredSet desired_;
	WtStreamScheduler scheduler_;
	WtChunkApplicationService application_;
	WtStoragePageCache page_cache_;
	WtChunkResourceCache resource_cache_;
	WtDesiredSetRuntimeService runtime_;
	WtEditRuntimeReplacementService edits_;
	WtEditSpatialIndex spatial_;
	RenderSink render_sink_;
	CollisionSink collision_sink_;
	std::vector<PendingReadiness> pending_;
	WtM5WorkloadMetrics metrics_;
};

} // namespace world_transvoxel::testing
