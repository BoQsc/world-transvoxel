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

void WorldTransvoxelTerrain::_physics_process(double delta) {
	(void)delta;
	drain_interaction_collision_publications_at_physics_boundary();
}

void WorldTransvoxelTerrain::
drain_interaction_collision_publications_at_physics_boundary() {
	if (!lifecycle_ || !application_ || !render_sink_ || !collision_sink_) return;
	const auto started = std::chrono::steady_clock::now();
	const std::uint64_t apply_time_start =
		application_->get_metrics().collision_apply_time_ns_total;
	std::size_t applied_items = 0;
	bool consumed_publication = false;
	const std::size_t publication_attempt_capacity =
		collision_apply_budget_ > std::numeric_limits<std::size_t>::max() / 3U ?
			collision_apply_budget_ : collision_apply_budget_ * 3U;
	for (std::size_t attempts = 0;
			attempts < publication_attempt_capacity &&
			applied_items < collision_apply_budget_; ++attempts) {
		const std::uint64_t used_ns =
			application_->get_metrics().collision_apply_time_ns_total -
			apply_time_start;
		if (collision_apply_deadline_ns_ != 0U &&
			used_ns >= collision_apply_deadline_ns_) {
			break;
		}
		const std::uint64_t remaining_ns =
			collision_apply_deadline_ns_ == 0U ? 0U :
			collision_apply_deadline_ns_ > used_ns ?
				collision_apply_deadline_ns_ - used_ns : 0U;
		WtApplicationBatchResult result =
			application_->apply_with_collision_deadline(
				0U, 1U, remaining_ns, *render_sink_, *collision_sink_
			);
		if (result.collision_processed == 0) {
			WtReadOnlyPublication publication;
			if (!lifecycle_->pop_interaction_collision_publication(publication)) {
				break;
			}
			consumed_publication = true;
			WtApplicationStatus submit_status = WtApplicationStatus::InvalidInput;
			if (publication.kind == WtReadOnlyPublicationKind::ExpectChunk) {
				submit_status = application_->expect_chunk(
					publication.key, publication.generation,
					publication.collision_required, publication.visual_required,
					publication.staged_replacement,
					publication.preserve_collision_ready,
					publication.world_revision,
					publication.independently_publishable_replacement
				);
				if (submit_status == WtApplicationStatus::Ok ||
					submit_status == WtApplicationStatus::AlreadyCurrent) {
					cancel_chunk_retirement(publication.key);
					if (publication.collision_required) {
						cancel_collision_retirement(publication.key);
					}
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
			} else if (publication.kind ==
					WtReadOnlyPublicationKind::SetCollisionRequired) {
				WtChunkApplicationRecord record;
				if (!application_->copy_record(publication.key, record) ||
					record.generation != publication.generation) {
					submit_status = WtApplicationStatus::StaleGeneration;
				} else {
					submit_status = application_->set_collision_required(
						publication.key, publication.collision_required
					);
					if (publication.collision_required &&
						(submit_status == WtApplicationStatus::Ok ||
						 submit_status == WtApplicationStatus::AlreadyCurrent)) {
						cancel_collision_retirement(publication.key);
					}
				}
			} else if (publication.kind ==
					WtReadOnlyPublicationKind::CollisionPayload &&
					publication.collision) {
				submit_status = application_->submit_collision(
					publication.collision, true
				);
			}
			lifecycle_->record_frontend_publication(
				publication, static_cast<std::int64_t>(submit_status)
			);
			if (submit_status != WtApplicationStatus::Ok &&
				submit_status != WtApplicationStatus::AlreadyCurrent) continue;
			if (publication.kind != WtReadOnlyPublicationKind::CollisionPayload) {
				continue;
			}
			const std::uint64_t submit_used_ns =
				application_->get_metrics().collision_apply_time_ns_total -
				apply_time_start;
			const std::uint64_t submit_remaining_ns =
				collision_apply_deadline_ns_ == 0U ? 0U :
				collision_apply_deadline_ns_ > submit_used_ns ?
					collision_apply_deadline_ns_ - submit_used_ns : 0U;
			result = application_->apply_with_collision_deadline(
				0U, 1U, submit_remaining_ns, *render_sink_, *collision_sink_
			);
		}
		if (result.collision_processed != 0) ++applied_items;
	}
	if (applied_items == 0 && !consumed_publication) return;
	if (applied_items != 0) {
		for (const WtChunkApplicationRecord &record :
				application_->get_records()) {
			const WtGenerationToken applied_generation =
				collision_sink_->applied_generation(record.key);
			if (applied_generation.value == 0) continue;
			lifecycle_->record_frontend_collision_residency(
				record.key,
				applied_generation,
				record.collision_world_revision
			);
		}
		publish_ready_independent_collision_coverage();
	}
	const std::uint64_t elapsed_ns = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - started
		).count()
	);
	++physics_boundary_collision_apply_calls_;
	physics_boundary_collision_apply_items_ += applied_items;
	physics_boundary_collision_apply_time_ns_total_ += elapsed_ns;
	physics_boundary_collision_apply_time_ns_maximum_ = std::max(
		physics_boundary_collision_apply_time_ns_maximum_, elapsed_ns
	);
	lifecycle_->notify_application_progress();
}

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
	const WtApplicationBatchResult applied =
		application_->apply_with_collision_deadline(
			render_apply_budget_,
			0U,
			0U,
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
	flush_ready_collision_retirements();
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

bool WorldTransvoxelTerrain::update_foreground_priority_lease(
	std::int64_t source_id,
	std::int64_t revision,
	std::int64_t priority_class,
	const godot::Array &chunk_coordinates
) {
	if (!lifecycle_ || source_id <= 0 || revision <= 0 ||
		priority_class < static_cast<std::int64_t>(
			WtForegroundPriorityClass::PlayerSupport
		) || priority_class > static_cast<std::int64_t>(
			WtForegroundPriorityClass::InteractionFocus
		) || static_cast<std::size_t>(chunk_coordinates.size()) >
			kWtForegroundPriorityKeysPerSource) {
		synchronous_world_error_ = "foreground priority lease is invalid";
		return false;
	}
	std::vector<WtChunkKey> keys;
	keys.reserve(static_cast<std::size_t>(chunk_coordinates.size()));
	for (std::int64_t index = 0; index < chunk_coordinates.size(); ++index) {
		const godot::Variant value = chunk_coordinates[index];
		if (value.get_type() != godot::Variant::VECTOR3I) {
			synchronous_world_error_ =
				"foreground priority lease is invalid";
			return false;
		}
		const godot::Vector3i coordinate = value;
		keys.push_back({ coordinate.x, coordinate.y, coordinate.z, 0 });
	}
	const WtReadOnlyRuntimeStatus status =
		lifecycle_->update_foreground_priority_lease({
			static_cast<std::uint64_t>(source_id),
			static_cast<std::uint64_t>(revision),
			static_cast<WtForegroundPriorityClass>(priority_class),
			std::move(keys),
		});
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
			const bool deferred_interaction_collision =
				deferred_publication_.interaction_critical &&
				deferred_publication_.kind ==
					WtReadOnlyPublicationKind::CollisionPayload;
			if (deferred_interaction_collision ||
					collision_publication_count != 0 ||
					!lifecycle_->pop_interaction_collision_publication(
						publication
					)) {
				publication = std::move(deferred_publication_);
				has_deferred_publication_ = false;
			}
		} else if (!lifecycle_->pop_non_collision_publication(publication)) {
			break;
		}
		const std::uint64_t collision_apply_time_ns_used =
			application_->get_metrics().collision_apply_time_ns_total -
			collision_apply_time_ns_start;
		const bool collision_deadline_reached =
			collision_apply_deadline_ns_ != 0U &&
			collision_apply_time_ns_used >= collision_apply_deadline_ns_;
		const bool gpu_resident_placeholder =
			publication.kind == WtReadOnlyPublicationKind::RenderPayload &&
			publication.render &&
			publication.render->publication_source ==
				WtRenderPublicationSource::GpuResidentPlaceholder;
		if ((publication.kind == WtReadOnlyPublicationKind::RenderPayload &&
				!gpu_resident_placeholder &&
				render_count >= render_apply_budget_) ||
			(publication.kind == WtReadOnlyPublicationKind::CollisionPayload &&
				((!publication.interaction_critical &&
					collision_publication_count >= collision_apply_budget_) ||
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
					publication.preserve_collision_ready,
					publication.world_revision,
					publication.independently_publishable_replacement
				);
				if (status == WtApplicationStatus::Ok ||
					status == WtApplicationStatus::AlreadyCurrent) {
					cancel_chunk_retirement(publication.key);
					if (publication.collision_required) {
						cancel_collision_retirement(publication.key);
					}
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
				if (status == WtApplicationStatus::Ok ||
						status == WtApplicationStatus::AlreadyCurrent) {
					if (publication.collision_required) {
						cancel_collision_retirement(publication.key);
					} else {
						stage_collision_retirement(publication.key);
					}
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
				if (gpu_resident_placeholder) {
					status = application_->apply_gpu_resident_placeholder(
						publication.render, *render_sink_
					);
				} else {
					++render_count;
					status = publication.render ?
						application_->submit_render(publication.render) :
						WtApplicationStatus::InvalidInput;
				}
				break;
			case WtReadOnlyPublicationKind::CollisionPayload:
				// Collision payloads are removed only by _physics_process().
				status = WtApplicationStatus::InvalidInput;
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
	cancel_collision_retirement(key);
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
		if (record.visual_required && !record.visual_ready &&
			render_sink_->applied_generation(record.key) != record.generation) {
			return;
		}
	}
	while (!pending_chunk_retirements_.empty()) {
		const WtChunkKey key = pending_chunk_retirements_.front();
		application_->forget_chunk(key);
		render_sink_->begin_render_retirement(key);
		collision_sink_->remove_collision(key);
		if (lifecycle_) {
			lifecycle_->record_frontend_collision_residency(key, {});
		}
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
	// Viewer supersession can forget an application record after its key moved
	// from the pending queue into the ready regional queue. Remove those keys
	// here as well: no producer remains that could prepare their visual branch,
	// and retaining one makes every later overlapping cohort wait forever.
	ready_staged_chunk_replacements_.erase(std::remove_if(
		ready_staged_chunk_replacements_.begin(),
		ready_staged_chunk_replacements_.end(),
		[this](const WtChunkKey &key) {
			WtChunkApplicationRecord record;
			return !application_->copy_record(key, record) ||
				!record.visual_required;
		}), ready_staged_chunk_replacements_.end());
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
		// External GPU activation and interactive collision publication commit
		// independently. Once both sinks already hold this exact generation, the
		// shared frontend marker has no publication work left. Detach it before
		// regional LOD closure so a completed edit cannot seed an unrelated swap.
		const bool visual_applied = !record.visual_required ||
			render_sink_->applied_generation(record.key) == record.generation;
		const bool collision_applied = !record.collision_required ||
			collision_sink_->applied_generation(record.key) == record.generation;
		if (visual_applied && collision_applied) {
			const auto independent = std::lower_bound(
				independently_publishable_chunk_replacements_.begin(),
				independently_publishable_chunk_replacements_.end(),
				*iterator
			);
			if (independent !=
					independently_publishable_chunk_replacements_.end() &&
					*independent == *iterator) {
				independently_publishable_chunk_replacements_.erase(independent);
			}
			iterator = pending_chunk_replacements_.erase(iterator);
			++completed_split_replacements_detached_;
			continue;
		}
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
			if (lifecycle_) {
				WtChunkApplicationRecord record;
				application_->copy_record(*iterator, record);
				lifecycle_->record_frontend_collision_residency(
					*iterator,
					collision_sink_->applied_generation(*iterator),
					record.collision_world_revision
				);
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
		if (lifecycle_) {
			for (const WtChunkApplicationRecord &record :
					application_->get_records()) {
				lifecycle_->record_frontend_collision_residency(
					record.key,
					collision_sink_->applied_generation(record.key),
					record.collision_world_revision
				);
			}
		}
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
	physics_boundary_collision_apply_calls_ = 0;
	physics_boundary_collision_apply_items_ = 0;
	physics_boundary_collision_apply_time_ns_total_ = 0;
	physics_boundary_collision_apply_time_ns_maximum_ = 0;
	const std::size_t staging_capacity = capacity <=
		std::numeric_limits<std::size_t>::max() / 2U ?
		capacity * 2U : std::numeric_limits<std::size_t>::max();
	pending_chunk_retirements_.clear();
	pending_chunk_retirements_.reserve(staging_capacity);
	pending_collision_retirements_.clear();
	pending_collision_retirements_.reserve(staging_capacity);
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
	completed_split_replacements_detached_ = 0;
	application_ = std::make_unique<WtChunkApplicationService>(
		staging_capacity,
		capacity,
		capacity
	);
}

} // namespace world_transvoxel
