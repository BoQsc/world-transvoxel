#include "api/world_transvoxel_terrain.h"

#include "physics/wt_godot_collision_sink.h"
#include "render/wt_godot_render_sink.h"
#include "services/wt_chunk_application.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace world_transvoxel {
namespace {

constexpr std::size_t kChunkRetirementFlushBudget = 4U;

} // namespace

void WorldTransvoxelTerrain::_process(double delta) {
	(void)delta;
	drain_world_publications();
	application_->apply(
		render_apply_budget_,
		collision_apply_budget_,
		*render_sink_,
		*collision_sink_
	);
	flush_ready_chunk_retirements();
	render_sink_->advance_retirements();
	notify_lifecycle_state();
}

bool WorldTransvoxelTerrain::update_viewer(
	std::int64_t viewer_id,
	std::int64_t revision,
	const godot::Vector3 &position,
	std::int64_t radius_chunks,
	std::int64_t maximum_lod
) {
	if (!lifecycle_ || viewer_id <= 0 || revision <= 0 ||
		radius_chunks < 0 ||
		radius_chunks > std::numeric_limits<std::uint32_t>::max() ||
		maximum_lod < 0 || maximum_lod > kWtMaximumLod ||
		!std::isfinite(static_cast<double>(position.x)) ||
		!std::isfinite(static_cast<double>(position.y)) ||
		!std::isfinite(static_cast<double>(position.z))) {
		synchronous_world_error_ = "viewer event is invalid";
		return false;
	}
	const WtReadOnlyRuntimeStatus status = lifecycle_->update_viewer(
		WtViewerSnapshot {
			static_cast<std::uint64_t>(viewer_id),
			static_cast<double>(position.x),
			static_cast<double>(position.y),
			static_cast<double>(position.z),
			static_cast<std::uint64_t>(revision),
		},
		static_cast<std::uint32_t>(radius_chunks),
		static_cast<std::uint8_t>(maximum_lod)
	);
	synchronous_world_error_ = wt_read_only_runtime_status_message(status);
	return status == WtReadOnlyRuntimeStatus::Ok;
}

bool WorldTransvoxelTerrain::remove_viewer(
	std::int64_t viewer_id,
	std::int64_t revision
) {
	if (!lifecycle_ || viewer_id <= 0 || revision <= 0) {
		synchronous_world_error_ = "viewer event is invalid";
		return false;
	}
	const WtReadOnlyRuntimeStatus status = lifecycle_->remove_viewer(
		static_cast<std::uint64_t>(viewer_id),
		static_cast<std::uint64_t>(revision)
	);
	synchronous_world_error_ = wt_read_only_runtime_status_message(status);
	return status == WtReadOnlyRuntimeStatus::Ok;
}

std::int64_t
WorldTransvoxelTerrain::get_rendered_chunk_count() const noexcept {
	return static_cast<std::int64_t>(render_sink_->resource_count());
}

std::int64_t
WorldTransvoxelTerrain::get_collision_chunk_count() const noexcept {
	return static_cast<std::int64_t>(collision_sink_->resource_count());
}

void WorldTransvoxelTerrain::drain_world_publications() {
	if (!lifecycle_) return;
	std::size_t render_count = 0;
	std::size_t collision_count = 0;
	for (std::size_t count = 0; count < 256U; ++count) {
		WtReadOnlyPublication publication;
		if (has_deferred_publication_) {
			publication = std::move(deferred_publication_);
			has_deferred_publication_ = false;
		} else if (!lifecycle_->pop_publication(publication)) {
			break;
		}
		if ((publication.kind == WtReadOnlyPublicationKind::RenderPayload &&
				render_count >= render_apply_budget_) ||
			(publication.kind == WtReadOnlyPublicationKind::CollisionPayload &&
				collision_count >= collision_apply_budget_)) {
			deferred_publication_ = std::move(publication);
			has_deferred_publication_ = true;
			break;
		}
		WtApplicationStatus status = WtApplicationStatus::Ok;
		switch (publication.kind) {
			case WtReadOnlyPublicationKind::ExpectChunk:
				status = application_->expect_chunk(
					publication.key,
					publication.generation,
					publication.collision_required
				);
				if (status == WtApplicationStatus::Ok ||
					status == WtApplicationStatus::AlreadyCurrent) {
					cancel_chunk_retirement(publication.key);
				}
				break;
			case WtReadOnlyPublicationKind::SetCollisionRequired: {
				const WtChunkApplicationRecord *record =
					application_->find_record(publication.key);
				if (record == nullptr ||
					record->generation != publication.generation) {
					status = WtApplicationStatus::StaleGeneration;
					break;
				}
				status = application_->set_collision_required(
					publication.key,
					publication.collision_required
				);
				if (!publication.collision_required) {
					collision_sink_->remove_collision(publication.key);
				}
				break;
			}
			case WtReadOnlyPublicationKind::RemoveChunk:
				stage_chunk_retirement(publication.key);
				break;
			case WtReadOnlyPublicationKind::RenderPayload:
				++render_count;
				status = publication.render ?
					application_->submit_render(publication.render) :
					WtApplicationStatus::InvalidInput;
				break;
			case WtReadOnlyPublicationKind::CollisionPayload:
				++collision_count;
				status = publication.collision ?
					application_->submit_collision(publication.collision) :
					WtApplicationStatus::InvalidInput;
				break;
			case WtReadOnlyPublicationKind::EditCommitted:
				synchronous_world_error_ = "ok";
				emit_signal(
					"edit_committed",
					static_cast<std::int64_t>(publication.world_revision)
				);
				break;
			case WtReadOnlyPublicationKind::EditRejected:
				synchronous_world_error_ =
					wt_read_only_edit_status_message(publication.edit_status);
				emit_signal("edit_failed", synchronous_world_error_);
				break;
			case WtReadOnlyPublicationKind::AuthoritativeSampleReady: {
				godot::Ref<WorldTransvoxelSample> sample;
				sample.instantiate();
				sample->set_sample(publication.authoritative_sample);
				synchronous_world_error_ = "ok";
				emit_signal(
					"authoritative_sample_ready",
					static_cast<std::int64_t>(publication.request_id),
					sample
				);
				break;
			}
			case WtReadOnlyPublicationKind::AuthoritativeSampleRejected:
				synchronous_world_error_ =
					wt_authoritative_sample_query_status_message(
						publication.sample_status
					);
				emit_signal(
					"authoritative_sample_failed",
					static_cast<std::int64_t>(publication.request_id),
					synchronous_world_error_
				);
				break;
			case WtReadOnlyPublicationKind::AuthoritativeSampleBatchReady: {
				godot::Array samples;
				samples.resize(
					static_cast<int>(publication.authoritative_samples.size())
				);
				for (std::size_t index = 0;
						index < publication.authoritative_samples.size();
						++index) {
					godot::Ref<WorldTransvoxelSample> sample;
					sample.instantiate();
					sample->set_sample(publication.authoritative_samples[index]);
					samples[static_cast<int>(index)] = sample;
				}
				synchronous_world_error_ = "ok";
				emit_signal(
					"authoritative_samples_ready",
					static_cast<std::int64_t>(publication.request_id),
					samples
				);
				break;
			}
			case WtReadOnlyPublicationKind::AuthoritativeSampleBatchRejected:
				synchronous_world_error_ =
					wt_authoritative_sample_query_status_message(
						publication.sample_status
					);
				emit_signal(
					"authoritative_samples_failed",
					static_cast<std::int64_t>(publication.request_id),
					synchronous_world_error_
				);
				break;
			case WtReadOnlyPublicationKind::WorldSnapshotReady: {
				const std::string utf8 =
					publication.snapshot_manifest_path.u8string();
				synchronous_world_error_ = "ok";
				emit_signal(
					"world_snapshot_ready",
					static_cast<std::int64_t>(publication.request_id),
					godot::String::utf8(utf8.c_str()),
					static_cast<std::int64_t>(
						publication.snapshot_source_revision
					),
					static_cast<std::int64_t>(publication.world_revision),
					static_cast<std::int64_t>(
						publication.snapshot_page_count
					)
				);
				break;
			}
			case WtReadOnlyPublicationKind::WorldSnapshotRejected:
				synchronous_world_error_ =
					wt_world_snapshot_store_status_message(
						publication.snapshot_status
					);
				emit_signal(
					"world_snapshot_failed",
					static_cast<std::int64_t>(publication.request_id),
					synchronous_world_error_
				);
				break;
		}
		if (status != WtApplicationStatus::Ok &&
			status != WtApplicationStatus::AlreadyCurrent &&
			status != WtApplicationStatus::StaleGeneration &&
			status != WtApplicationStatus::NotFound) {
			synchronous_world_error_ =
				"world publication application failed";
		}
	}
}

void WorldTransvoxelTerrain::stage_chunk_retirement(
	const WtChunkKey &key
) {
	const auto iterator = std::lower_bound(
		pending_chunk_retirements_.begin(),
		pending_chunk_retirements_.end(),
		key
	);
	if (iterator == pending_chunk_retirements_.end() || *iterator != key) {
		pending_chunk_retirements_.insert(iterator, key);
	}
}

void WorldTransvoxelTerrain::cancel_chunk_retirement(
	const WtChunkKey &key
) {
	const auto iterator = std::lower_bound(
		pending_chunk_retirements_.begin(),
		pending_chunk_retirements_.end(),
		key
	);
	if (iterator != pending_chunk_retirements_.end() && *iterator == key) {
		pending_chunk_retirements_.erase(iterator);
	}
}

void WorldTransvoxelTerrain::flush_ready_chunk_retirements() {
	if (pending_chunk_retirements_.empty()) return;
	for (const WtChunkApplicationRecord &record : application_->get_records()) {
		if (std::binary_search(
			pending_chunk_retirements_.begin(),
			pending_chunk_retirements_.end(),
			record.key
		)) {
			continue;
		}
		if (!record.fully_ready()) return;
	}
	std::size_t retired_count = 0;
	while (
		retired_count < kChunkRetirementFlushBudget &&
		!pending_chunk_retirements_.empty()
	) {
		const WtChunkKey key = pending_chunk_retirements_.front();
		application_->forget_chunk(key);
		render_sink_->begin_render_retirement(key);
		collision_sink_->remove_collision(key);
		pending_chunk_retirements_.erase(pending_chunk_retirements_.begin());
		++retired_count;
	}
}

void WorldTransvoxelTerrain::reset_world_application(std::size_t capacity) {
	has_deferred_publication_ = false;
	deferred_publication_ = {};
	const std::size_t staging_capacity = capacity <=
		std::numeric_limits<std::size_t>::max() / 2U ?
		capacity * 2U : std::numeric_limits<std::size_t>::max();
	pending_chunk_retirements_.clear();
	pending_chunk_retirements_.reserve(staging_capacity);
	application_ = std::make_unique<WtChunkApplicationService>(
		staging_capacity,
		capacity,
		capacity
	);
}

} // namespace world_transvoxel
