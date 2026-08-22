#include "api/world_transvoxel_terrain.h"

#include "physics/wt_godot_collision_sink.h"
#include "render/wt_godot_render_sink.h"
#include "services/wt_chunk_application.h"
#include "services/wt_chunk_publication_policy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace world_transvoxel {
void WorldTransvoxelTerrain::_process(double delta) {
	(void)delta;
	const WtApplicationMetrics application_before =
		application_->get_metrics();
	std::size_t collision_publication_count = 0;
	const bool drained_publications = drain_world_publications(
		collision_publication_count,
		application_before.collision_apply_time_ns_total
	);
	update_visibility_staging_state();
	const WtApplicationMetrics application_after_drain =
		application_->get_metrics();
	const std::uint64_t collision_apply_time_ns_used =
		application_after_drain.collision_apply_time_ns_total -
		application_before.collision_apply_time_ns_total;
	const std::size_t collision_apply_budget_remaining =
		collision_publication_count < collision_apply_budget_ ?
		collision_apply_budget_ - collision_publication_count : 0U;
	const std::uint64_t collision_apply_deadline_ns_remaining =
		collision_apply_deadline_ns_ == 0U ? 0U :
		collision_apply_time_ns_used < collision_apply_deadline_ns_ ?
		collision_apply_deadline_ns_ - collision_apply_time_ns_used : 0U;
	const std::size_t bounded_collision_apply_budget =
		collision_apply_deadline_ns_ == 0U ||
			collision_apply_deadline_ns_remaining != 0U ?
		collision_apply_budget_remaining : 0U;
	const WtApplicationBatchResult applied =
		application_->apply_with_collision_deadline(
			render_apply_budget_,
			bounded_collision_apply_budget,
			collision_apply_deadline_ns_remaining,
			*render_sink_,
			*collision_sink_
		);
	const WtApplicationMetrics application_after =
		application_->get_metrics();
	collision_apply_frame_time_ns_last_ =
		application_after.collision_apply_time_ns_total -
		application_before.collision_apply_time_ns_total;
	collision_apply_frame_items_last_ =
		application_after.applied_collision -
		application_before.applied_collision;
	collision_apply_frame_time_ns_total_ +=
		collision_apply_frame_time_ns_last_;
	collision_apply_frame_time_ns_maximum_ = std::max(
		collision_apply_frame_time_ns_maximum_,
		collision_apply_frame_time_ns_last_
	);
	collision_apply_frame_items_maximum_ = std::max(
		collision_apply_frame_items_maximum_,
		collision_apply_frame_items_last_
	);
	if (collision_apply_frame_items_last_ != 0 &&
		collision_apply_frame_time_ns_last_ >
			collision_apply_deadline_ns_) {
		++collision_apply_frame_deadline_overruns_;
	}
	if (lifecycle_ && (drained_publications || applied.render_processed != 0)) {
		lifecycle_->notify_application_progress();
	}
	flush_ready_independent_publication_regions();
	flush_ready_chunk_retirements();
	flush_ready_chunk_replacements();
	flush_ready_render_retirements();
	publish_staged_records_if_ready();
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

bool WorldTransvoxelTerrain::update_collision_viewer(
	std::int64_t viewer_id,
	std::int64_t revision,
	const godot::Vector3 &position,
	std::int64_t radius_chunks
) {
	if (!lifecycle_ || viewer_id <= 0 || revision <= 0 ||
		radius_chunks < 0 ||
		radius_chunks > std::numeric_limits<std::uint32_t>::max() ||
		!std::isfinite(static_cast<double>(position.x)) ||
		!std::isfinite(static_cast<double>(position.y)) ||
		!std::isfinite(static_cast<double>(position.z))) {
		synchronous_world_error_ = "collision viewer event is invalid";
		return false;
	}
	const WtReadOnlyRuntimeStatus status =
		lifecycle_->update_collision_viewer(
			WtViewerSnapshot {
				static_cast<std::uint64_t>(viewer_id),
				static_cast<double>(position.x),
				static_cast<double>(position.y),
				static_cast<double>(position.z),
				static_cast<std::uint64_t>(revision),
			},
			static_cast<std::uint32_t>(radius_chunks)
		);
	synchronous_world_error_ = wt_read_only_runtime_status_message(status);
	return status == WtReadOnlyRuntimeStatus::Ok;
}

bool WorldTransvoxelTerrain::remove_collision_viewer(
	std::int64_t viewer_id,
	std::int64_t revision
) {
	if (!lifecycle_ || viewer_id <= 0 || revision <= 0) {
		synchronous_world_error_ = "collision viewer event is invalid";
		return false;
	}
	const WtReadOnlyRuntimeStatus status =
		lifecycle_->remove_collision_viewer(
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

bool WorldTransvoxelTerrain::drain_world_publications(
	std::size_t &collision_publication_count,
	std::uint64_t collision_apply_time_ns_start
) {
	if (!lifecycle_) return false;
	std::size_t render_count = 0;
	bool drained = false;
	for (std::size_t count = 0; count < 256U; ++count) {
		WtReadOnlyPublication publication;
		if (has_deferred_publication_) {
			publication = std::move(deferred_publication_);
			has_deferred_publication_ = false;
		} else if (!lifecycle_->pop_publication(publication)) {
			break;
		}
		const std::uint64_t collision_apply_time_ns_used =
			application_->get_metrics().collision_apply_time_ns_total -
			collision_apply_time_ns_start;
		const bool collision_deadline_reached =
			collision_apply_deadline_ns_ != 0U &&
			collision_apply_time_ns_used >= collision_apply_deadline_ns_;
		if ((publication.kind == WtReadOnlyPublicationKind::RenderPayload &&
				render_count >= render_apply_budget_) ||
			(publication.kind == WtReadOnlyPublicationKind::CollisionPayload &&
				(collision_publication_count >= collision_apply_budget_ ||
					collision_deadline_reached))) {
			deferred_publication_ = std::move(publication);
			has_deferred_publication_ = true;
			if (!lifecycle_->pop_unbudgeted_publication(publication)) break;
		}
		drained = true;
		WtApplicationStatus status = WtApplicationStatus::Ok;
		bool collision_deadline_exhausted = false;
		switch (publication.kind) {
			case WtReadOnlyPublicationKind::ExpectChunk:
				status = application_->expect_chunk(
					publication.key,
					publication.generation,
					publication.collision_required,
					publication.visual_required,
					publication.staged_replacement,
					publication.preserve_collision_ready
				);
				if (status == WtApplicationStatus::Ok ||
					status == WtApplicationStatus::AlreadyCurrent) {
					cancel_chunk_retirement(publication.key);
					if (publication.visual_required) {
						cancel_render_retirement(publication.key);
					}
					if (publication.staged_replacement) {
						stage_chunk_replacement(
							publication.key,
							publication.independently_publishable_replacement
						);
					}
				}
				break;
			case WtReadOnlyPublicationKind::SetCollisionRequired: {
				WtChunkApplicationRecord record;
				if (!application_->copy_record(publication.key, record) ||
					record.generation != publication.generation) {
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
			case WtReadOnlyPublicationKind::SetVisualRequired: {
				WtChunkApplicationRecord record;
				if (!application_->copy_record(publication.key, record) ||
					record.generation != publication.generation) {
					status = WtApplicationStatus::StaleGeneration;
					break;
				}
				status = application_->set_visual_required(
					publication.key,
					publication.visual_required
				);
				if (publication.visual_required) {
					cancel_render_retirement(publication.key);
				} else {
					stage_render_retirement(publication.key);
				}
				break;
			}
			case WtReadOnlyPublicationKind::RemoveChunk:
				cancel_chunk_replacement(publication.key);
				stage_chunk_retirement(publication.key);
				break;
			case WtReadOnlyPublicationKind::RenderPayload:
				++render_count;
				status = publication.render ?
					application_->submit_render(publication.render) :
					WtApplicationStatus::InvalidInput;
				break;
			case WtReadOnlyPublicationKind::CollisionPayload:
				++collision_publication_count;
				status = publication.collision ?
					application_->submit_collision(publication.collision) :
					WtApplicationStatus::InvalidInput;
				// Collision publications are already bounded by
				// collision_apply_budget_. Apply each accepted payload before
				// consuming a later same-key ExpectChunk publication so a
				// transition remesh cannot make the safety collision stale before
				// it reaches the physics server. The previous shape remains active
				// there until the replacement generation is ready.
				if (status == WtApplicationStatus::Ok) {
					const std::uint64_t remaining_deadline_ns =
						collision_apply_deadline_ns_ == 0U ? 0U :
						collision_apply_time_ns_used <
								collision_apply_deadline_ns_ ?
							collision_apply_deadline_ns_ -
								collision_apply_time_ns_used :
							0U;
					application_->apply_with_collision_deadline(
						0U,
						1U,
						remaining_deadline_ns,
						*render_sink_,
						*collision_sink_
					);
					const std::uint64_t total_collision_apply_time_ns =
						application_->get_metrics().
							collision_apply_time_ns_total -
						collision_apply_time_ns_start;
					if (collision_apply_deadline_ns_ != 0U &&
						total_collision_apply_time_ns >=
							collision_apply_deadline_ns_) {
						collision_deadline_exhausted = true;
					}
				}
				break;
			case WtReadOnlyPublicationKind::ViewerPlanStarted:
				if (open_viewer_plan_publications_ !=
						std::numeric_limits<std::uint32_t>::max()) {
					++open_viewer_plan_publications_;
				}
				break;
			case WtReadOnlyPublicationKind::ViewerPlanCompleted:
				if (open_viewer_plan_publications_ != 0) {
					--open_viewer_plan_publications_;
				}
				latest_completed_viewer_plan_revision_ = std::max(
					latest_completed_viewer_plan_revision_,
					publication.world_revision
				);
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
		lifecycle_->record_frontend_publication(
			publication,
			static_cast<std::int64_t>(status)
		);
		if (collision_deadline_exhausted) {
			break;
		}
	}
	return drained;
}

void WorldTransvoxelTerrain::stage_chunk_retirement(
	const WtChunkKey &key
) {
	cancel_chunk_replacement(key);
	cancel_render_retirement(key);
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

void WorldTransvoxelTerrain::stage_chunk_replacement(
	const WtChunkKey &key,
	bool independently_publishable
) {
	const auto ready = std::lower_bound(
		ready_staged_chunk_replacements_.begin(),
		ready_staged_chunk_replacements_.end(),
		key
	);
	if (ready != ready_staged_chunk_replacements_.end() && *ready == key) {
		ready_staged_chunk_replacements_.erase(ready);
	}
	const auto iterator = std::lower_bound(
		pending_chunk_replacements_.begin(),
		pending_chunk_replacements_.end(),
		key
	);
	if (iterator == pending_chunk_replacements_.end() || *iterator != key) {
		pending_chunk_replacements_.insert(iterator, key);
	}
	if (!independently_publishable) return;
	const auto independent = std::lower_bound(
		independently_publishable_chunk_replacements_.begin(),
		independently_publishable_chunk_replacements_.end(),
		key
	);
	if (independent ==
			independently_publishable_chunk_replacements_.end() ||
			*independent != key) {
		independently_publishable_chunk_replacements_.insert(
			independent,
			key
		);
	}
}

void WorldTransvoxelTerrain::cancel_chunk_replacement(
	const WtChunkKey &key
) {
	clear_visibility_coverage_priority_request(key);
	const auto iterator = std::lower_bound(
		pending_chunk_replacements_.begin(),
		pending_chunk_replacements_.end(),
		key
	);
	if (iterator != pending_chunk_replacements_.end() && *iterator == key) {
		pending_chunk_replacements_.erase(iterator);
	}
	const auto ready = std::lower_bound(
		ready_staged_chunk_replacements_.begin(),
		ready_staged_chunk_replacements_.end(),
		key
	);
	if (ready != ready_staged_chunk_replacements_.end() && *ready == key) {
		ready_staged_chunk_replacements_.erase(ready);
	}
	const auto independent = std::lower_bound(
		independently_publishable_chunk_replacements_.begin(),
		independently_publishable_chunk_replacements_.end(),
		key
	);
	if (independent !=
			independently_publishable_chunk_replacements_.end() &&
			*independent == key) {
		independently_publishable_chunk_replacements_.erase(independent);
	}
}

void WorldTransvoxelTerrain::stage_render_retirement(
	const WtChunkKey &key
) {
	const auto iterator = std::lower_bound(
		pending_render_retirements_.begin(),
		pending_render_retirements_.end(),
		key
	);
	if (iterator == pending_render_retirements_.end() || *iterator != key) {
		pending_render_retirements_.insert(iterator, key);
	}
}

void WorldTransvoxelTerrain::cancel_render_retirement(
	const WtChunkKey &key
) {
	const auto iterator = std::lower_bound(
		pending_render_retirements_.begin(),
		pending_render_retirements_.end(),
		key
	);
	if (iterator != pending_render_retirements_.end() && *iterator == key) {
		pending_render_retirements_.erase(iterator);
	}
}

void WorldTransvoxelTerrain::flush_ready_chunk_retirements() {
	if (open_viewer_plan_publications_ != 0) return;
	if (pending_chunk_retirements_.empty()) return;
	if (!pending_chunk_replacements_.empty()) return;
	for (const WtChunkApplicationRecord &record : application_->get_records()) {
		if (std::binary_search(
			pending_chunk_retirements_.begin(),
			pending_chunk_retirements_.end(),
			record.key
		)) {
			continue;
		}
		if (record.visual_required && !record.visual_ready) return;
	}
	while (!pending_chunk_retirements_.empty()) {
		const WtChunkKey key = pending_chunk_retirements_.front();
		application_->forget_chunk(key);
		render_sink_->begin_render_retirement(key);
		collision_sink_->remove_collision(key);
		pending_chunk_retirements_.erase(pending_chunk_retirements_.begin());
	}
}

void WorldTransvoxelTerrain::flush_ready_render_retirements() {
	if (open_viewer_plan_publications_ != 0) return;
	if (!pending_chunk_replacements_.empty() ||
		!pending_chunk_retirements_.empty()) {
		return;
	}
	while (!pending_render_retirements_.empty()) {
		const WtChunkKey key = pending_render_retirements_.front();
		render_sink_->begin_render_retirement(key);
		pending_render_retirements_.erase(
			pending_render_retirements_.begin()
		);
	}
}

void WorldTransvoxelTerrain::flush_ready_chunk_replacements() {
	for (auto iterator = pending_chunk_replacements_.begin();
			iterator != pending_chunk_replacements_.end();) {
		WtChunkApplicationRecord record;
		if (!application_->copy_record(*iterator, record)) {
			const WtChunkKey key = *iterator;
			iterator = pending_chunk_replacements_.erase(iterator);
			const auto independent = std::lower_bound(
				independently_publishable_chunk_replacements_.begin(),
				independently_publishable_chunk_replacements_.end(),
				key
			);
			if (independent !=
					independently_publishable_chunk_replacements_.end() &&
					*independent == key) {
				independently_publishable_chunk_replacements_.erase(
					independent
				);
			}
			continue;
		}
		if (!record.fully_ready()) {
			++iterator;
			continue;
		}
		clear_visibility_coverage_priority_request(record.key);
		if (cpu_causal_trace_active_ && lifecycle_) {
			lifecycle_->record_frontend_visibility(
				WtCausalTraceEventKind::VisibilityReplacementReady,
				&record.key,
				record.generation,
				pending_chunk_replacements_.size(),
				pending_chunk_retirements_.size(),
				static_cast<std::int64_t>(pending_render_retirements_.size())
			);
		}
		const auto independent = std::lower_bound(
			independently_publishable_chunk_replacements_.begin(),
			independently_publishable_chunk_replacements_.end(),
			*iterator
		);
		if (independent !=
				independently_publishable_chunk_replacements_.end() &&
				*independent == *iterator) {
			const bool requires_regional_publication =
				wt_chunk_replacement_requires_regional_publication(
					*iterator,
					pending_chunk_retirements_
				);
			if (requires_regional_publication) {
				const WtChunkKey key = *iterator;
				const auto ready = std::lower_bound(
					ready_staged_chunk_replacements_.begin(),
					ready_staged_chunk_replacements_.end(),
					key
				);
				if (ready == ready_staged_chunk_replacements_.end() ||
						*ready != key) {
					ready_staged_chunk_replacements_.insert(ready, key);
				}
				iterator = pending_chunk_replacements_.erase(iterator);
				continue;
			}
			if (!render_sink_->publish_staged_record(*iterator) ||
					!collision_sink_->publish_staged_record(*iterator)) {
				++iterator;
				continue;
			}
			independently_publishable_chunk_replacements_.erase(independent);
		} else {
			const WtChunkKey key = *iterator;
			const auto ready = std::lower_bound(
				ready_staged_chunk_replacements_.begin(),
				ready_staged_chunk_replacements_.end(),
				key
			);
			if (ready == ready_staged_chunk_replacements_.end() ||
					*ready != key) {
				ready_staged_chunk_replacements_.insert(ready, key);
			}
		}
		iterator = pending_chunk_replacements_.erase(iterator);
	}
}

void WorldTransvoxelTerrain::update_visibility_staging_state() {
	if (pending_chunk_retirements_.empty() &&
			pending_chunk_replacements_.empty() &&
			pending_render_retirements_.empty()) {
		render_sink_->set_visibility_staging_reference_chunks({});
		render_sink_->set_new_record_visibility_staging_enabled(false);
		collision_sink_->set_staging_reference_chunks({});
		collision_sink_->set_new_record_staging_enabled(false);
		return;
	}
	std::vector<WtChunkKey> references = pending_chunk_retirements_;
	references.insert(
		references.end(),
		pending_chunk_replacements_.begin(),
		pending_chunk_replacements_.end()
	);
	references.insert(
		references.end(),
		ready_staged_chunk_replacements_.begin(),
		ready_staged_chunk_replacements_.end()
	);
	references.insert(
		references.end(),
		pending_render_retirements_.begin(),
		pending_render_retirements_.end()
	);
	std::sort(references.begin(), references.end());
	references.erase(
		std::unique(references.begin(), references.end()),
		references.end()
	);
	render_sink_->set_visibility_staging_reference_chunks(references);
	render_sink_->set_new_record_visibility_staging_enabled(true);
	collision_sink_->set_staging_reference_chunks(references);
	collision_sink_->set_new_record_staging_enabled(true);
}

void WorldTransvoxelTerrain::publish_staged_records_if_ready() {
	if (open_viewer_plan_publications_ != 0) return;
	if (cpu_causal_trace_active_ && lifecycle_ &&
		(trace_pending_replacements_ != pending_chunk_replacements_.size() ||
			trace_pending_retirements_ != pending_chunk_retirements_.size() ||
			trace_pending_render_retirements_ !=
				pending_render_retirements_.size())) {
		trace_pending_replacements_ = pending_chunk_replacements_.size();
		trace_pending_retirements_ = pending_chunk_retirements_.size();
		trace_pending_render_retirements_ = pending_render_retirements_.size();
		if (trace_pending_replacements_ != 0 ||
			trace_pending_retirements_ != 0 ||
			trace_pending_render_retirements_ != 0) {
			lifecycle_->record_frontend_visibility(
				WtCausalTraceEventKind::VisibilityStagingBlocked,
				nullptr,
				{},
				trace_pending_replacements_,
				trace_pending_retirements_,
				static_cast<std::int64_t>(
					trace_pending_render_retirements_
				)
			);
		}
	}
	if (!pending_chunk_retirements_.empty()) {
		return;
	}
	if (!pending_chunk_replacements_.empty()) {
		return;
	}
	if (!pending_render_retirements_.empty()) {
		return;
	}
	render_sink_->set_new_record_visibility_staging_enabled(false);
	render_sink_->set_visibility_staging_reference_chunks({});
	collision_sink_->set_new_record_staging_enabled(false);
	collision_sink_->set_staging_reference_chunks({});
	const std::size_t staged_render_count = render_sink_->staged_count();
	const std::size_t staged_collision_count = collision_sink_->staged_count();
	if (render_sink_->has_staged_records()) {
		render_sink_->publish_staged_records();
	}
	if (collision_sink_->has_staged_records()) {
		collision_sink_->publish_staged_records();
	}
	ready_staged_chunk_replacements_.clear();
	if (cpu_causal_trace_active_ && lifecycle_ &&
		(staged_render_count != 0 || staged_collision_count != 0)) {
		lifecycle_->record_frontend_visibility(
			WtCausalTraceEventKind::VisibilityBatchPublished,
			nullptr,
			{},
			staged_render_count,
			staged_collision_count,
			0
		);
	}
}

void WorldTransvoxelTerrain::reset_world_application(std::size_t capacity) {
	has_deferred_publication_ = false;
	deferred_publication_ = {};
	collision_apply_frame_time_ns_last_ = 0;
	collision_apply_frame_time_ns_total_ = 0;
	collision_apply_frame_time_ns_maximum_ = 0;
	collision_apply_frame_items_last_ = 0;
	collision_apply_frame_items_maximum_ = 0;
	collision_apply_frame_deadline_overruns_ = 0;
	const std::size_t staging_capacity = capacity <=
		std::numeric_limits<std::size_t>::max() / 2U ?
		capacity * 2U : std::numeric_limits<std::size_t>::max();
	pending_chunk_retirements_.clear();
	pending_chunk_retirements_.reserve(staging_capacity);
	pending_chunk_replacements_.clear();
	pending_chunk_replacements_.reserve(staging_capacity);
	ready_staged_chunk_replacements_.clear();
	ready_staged_chunk_replacements_.reserve(staging_capacity);
	independently_publishable_chunk_replacements_.clear();
	independently_publishable_chunk_replacements_.reserve(staging_capacity);
	visibility_coverage_priority_requests_.clear();
	visibility_coverage_priority_requests_.reserve(staging_capacity);
	pending_render_retirements_.clear();
	pending_render_retirements_.reserve(staging_capacity);
	open_viewer_plan_publications_ = 0;
	latest_completed_viewer_plan_revision_ = 0;
	regional_visibility_publications_ = 0;
	regional_visibility_replacements_ = 0;
	regional_visibility_retirements_ = 0;
	application_ = std::make_unique<WtChunkApplicationService>(
		staging_capacity,
		capacity,
		capacity
	);
}

} // namespace world_transvoxel
