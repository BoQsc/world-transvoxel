#include "services/wt_edit_runtime_replacement.h"

#include "services/wt_chunk_application.h"
#include "services/wt_chunk_resource_cache.h"
#include "services/wt_page_meshing_runtime_owner.h"
#include "storage/wt_storage_page_cache.h"
#include "streaming/wt_multi_viewer_desired_set.h"
#include "streaming/wt_stream_scheduler.h"

#include <algorithm>
#include <limits>

namespace world_transvoxel {

namespace {

std::int64_t floor_q16(std::int64_t value) noexcept {
	const std::int64_t quotient = value / kWtEditCoordinateScale;
	return value < 0 && value % kWtEditCoordinateScale != 0 ?
		quotient - 1 :
		quotient;
}

std::int64_t midpoint_q16(
	std::int64_t minimum,
	std::int64_t maximum
) noexcept {
	return minimum / 2 + maximum / 2 +
		(minimum % 2 + maximum % 2) / 2;
}

WtGridPoint command_center(const WtEditCommand &command) noexcept {
	if (command.shape == WtEditShape::Sphere) {
		return {
			floor_q16(command.sphere.center_x_q16),
			floor_q16(command.sphere.center_y_q16),
			floor_q16(command.sphere.center_z_q16),
		};
	}
	return {
		floor_q16(midpoint_q16(
			command.box.minimum_x_q16,
			command.box.maximum_x_q16
		)),
		floor_q16(midpoint_q16(
			command.box.minimum_y_q16,
			command.box.maximum_y_q16
		)),
		floor_q16(midpoint_q16(
			command.box.minimum_z_q16,
			command.box.maximum_z_q16
		)),
	};
}

bool contains_point(
	const WtChunkBounds &bounds,
	const WtGridPoint &point
) noexcept {
	return point.x >= bounds.minimum.x && point.x < bounds.maximum.x &&
		point.y >= bounds.minimum.y && point.y < bounds.maximum.y &&
		point.z >= bounds.minimum.z && point.z < bounds.maximum.z;
}

bool contains_command_center(
	const WtChunkKey &key,
	const WtEditTransaction &transaction
) noexcept {
	const WtChunkBounds bounds = wt_chunk_bounds(key);
	for (const WtEditCommand &command : transaction.commands) {
		if (contains_point(bounds, command_center(command))) return true;
	}
	return false;
}

bool intersects_owned_cells(
	const WtChunkKey &key,
	const WtEditTransaction &transaction
) noexcept {
	const WtChunkBounds bounds = wt_chunk_bounds(key);
	for (const WtEditCommand &command : transaction.commands) {
		if (command.bounds.maximum.x >= bounds.minimum.x &&
			command.bounds.minimum.x < bounds.maximum.x &&
			command.bounds.maximum.y >= bounds.minimum.y &&
			command.bounds.minimum.y < bounds.maximum.y &&
			command.bounds.maximum.z >= bounds.minimum.z &&
			command.bounds.minimum.z < bounds.maximum.z) {
			return true;
		}
	}
	return false;
}

std::uint8_t command_dirty_regular_bricks(
	const WtChunkKey &key,
	const WtEditBounds &dirty
) noexcept {
	if (key.lod != 0) return 0xff;
	const WtGridPoint chunk_minimum = wt_chunk_bounds(key).minimum;
	std::uint8_t mask = 0;
	for (std::int32_t z = 0; z < 2; ++z) {
		for (std::int32_t y = 0; y < 2; ++y) {
			for (std::int32_t x = 0; x < 2; ++x) {
				const WtGridPoint minimum = {
					chunk_minimum.x + x * 8,
					chunk_minimum.y + y * 8,
					chunk_minimum.z + z * 8,
				};
				const WtGridPoint maximum = {
					minimum.x + 8, minimum.y + 8, minimum.z + 8,
				};
				if (dirty.maximum.x >= minimum.x && dirty.minimum.x <= maximum.x &&
					dirty.maximum.y >= minimum.y && dirty.minimum.y <= maximum.y &&
					dirty.maximum.z >= minimum.z && dirty.minimum.z <= maximum.z) {
					mask |= static_cast<std::uint8_t>(
						1U << static_cast<unsigned int>(x + y * 2 + z * 4)
					);
				}
			}
		}
	}
	return mask;
}

WtChunkEditDelta transaction_delta(
	const WtChunkKey &key,
	const WtEditTransaction &transaction
) noexcept {
	WtChunkEditDelta delta;
	const WtChunkBounds chunk = wt_chunk_bounds(key);
	const std::int64_t spacing = wt_lod_cell_size(key.lod);
	const auto subtract_saturated = [](std::int64_t value, std::int64_t amount) {
		return value < std::numeric_limits<std::int64_t>::min() + amount ?
			std::numeric_limits<std::int64_t>::min() : value - amount;
	};
	const auto add_saturated = [](std::int64_t value, std::int64_t amount) {
		return value > std::numeric_limits<std::int64_t>::max() - amount ?
			std::numeric_limits<std::int64_t>::max() : value + amount;
	};
	for (const WtEditCommand &command : transaction.commands) {
		const bool affects_dependency =
			command.bounds.maximum.x >= subtract_saturated(chunk.minimum.x, spacing) &&
			command.bounds.minimum.x <= add_saturated(chunk.maximum.x, spacing) &&
			command.bounds.maximum.y >= subtract_saturated(chunk.minimum.y, spacing) &&
			command.bounds.minimum.y <= add_saturated(chunk.maximum.y, spacing) &&
			command.bounds.maximum.z >= subtract_saturated(chunk.minimum.z, spacing) &&
			command.bounds.minimum.z <= add_saturated(chunk.maximum.z, spacing);
		if (!affects_dependency) continue;
		const std::uint8_t command_mask = command_dirty_regular_bricks(
			key, command.bounds
		);
		if (!delta.valid) {
			delta.dirty_minimum = command.bounds.minimum;
			delta.dirty_maximum = command.bounds.maximum;
			delta.valid = true;
		} else {
			delta.dirty_minimum.x = std::min(delta.dirty_minimum.x, command.bounds.minimum.x);
			delta.dirty_minimum.y = std::min(delta.dirty_minimum.y, command.bounds.minimum.y);
			delta.dirty_minimum.z = std::min(delta.dirty_minimum.z, command.bounds.minimum.z);
			delta.dirty_maximum.x = std::max(delta.dirty_maximum.x, command.bounds.maximum.x);
			delta.dirty_maximum.y = std::max(delta.dirty_maximum.y, command.bounds.maximum.y);
			delta.dirty_maximum.z = std::max(delta.dirty_maximum.z, command.bounds.maximum.z);
		}
		delta.dirty_regular_brick_mask |= command_mask;
	}
	// The spatial index owns the conservative dependency set. A valid zero-mask
	// delta means this chunk is present only to supply a halo or transition.
	if (!delta.valid) delta.valid = true;
	return delta;
}

std::uint64_t count_dirty_blocks(std::uint8_t mask) noexcept {
	std::uint64_t count = 0;
	while (mask != 0) {
		count += mask & 1U;
		mask = static_cast<std::uint8_t>(mask >> 1U);
	}
	return count;
}

} // namespace

WtEditRuntimeReplacementService::WtEditRuntimeReplacementService(
	std::size_t replacement_capacity
) :
		replacement_capacity_(replacement_capacity),
		valid_(replacement_capacity > 0 &&
			replacement_capacity <= kWtMaximumEditRuntimeReplacements) {
	if (valid_) {
		affected_.reserve(replacement_capacity);
		prepared_.reserve(replacement_capacity);
		last_replacements_.reserve(replacement_capacity);
	}
}

bool WtEditRuntimeReplacementService::valid() const noexcept {
	return valid_;
}

WtEditRuntimeReplacementStatus
WtEditRuntimeReplacementService::prepare_loaded_chunks(
	const WtEditTransaction &transaction,
	const WtEditSpatialIndex &spatial_index,
	const WtStreamScheduler &scheduler,
	const WtChunkApplicationService &application,
	const std::vector<WtDesiredChunk> *desired_chunks,
	const std::vector<WtChunkKey> *active_visual_chunks
) {
	++metrics_.transaction_attempts;
	affected_.clear();
	prepared_.clear();
	last_replacements_.clear();
	has_prepared_ = false;
	if (!valid_) {
		++metrics_.capacity_rejections;
		return WtEditRuntimeReplacementStatus::InvalidConfiguration;
	}
	if (spatial_index.query_transaction(transaction, affected_) !=
		WtEditSpatialStatus::Ok) {
		++metrics_.spatial_rejections;
		return WtEditRuntimeReplacementStatus::SpatialQueryFailed;
	}
	metrics_.queried_chunks += affected_.size();
	if (affected_.size() > replacement_capacity_) {
		++metrics_.capacity_rejections;
		return WtEditRuntimeReplacementStatus::AffectedCapacityExceeded;
	}
	if (affected_.empty()) {
		prepared_source_revision_ = transaction.source_revision;
		prepared_base_revision_ = transaction.base_revision;
		prepared_committed_revision_ = transaction.committed_revision;
		has_prepared_ = true;
		return WtEditRuntimeReplacementStatus::Ok;
	}

	for (const WtChunkKey &key : affected_) {
		const WtChunkRecord *record = scheduler.find_record(key);
		WtChunkApplicationRecord application_record;
		if (record == nullptr ||
			!application.copy_record(key, application_record) ||
			record->lifecycle == WtChunkLifecycle::Cancelled ||
			application_record.generation != record->generation) {
			++metrics_.state_rejections;
			return WtEditRuntimeReplacementStatus::RuntimeStateMismatch;
		}
		if (record->source_revision != transaction.source_revision) {
			++metrics_.state_rejections;
			return WtEditRuntimeReplacementStatus::SourceRevisionMismatch;
		}
		if (record->world_revision > transaction.base_revision) {
			++metrics_.state_rejections;
			return WtEditRuntimeReplacementStatus::WorldRevisionMismatch;
		}
		bool collision_required = application_record.collision_required;
		bool visual_required = application_record.visual_required;
		if (desired_chunks != nullptr) {
			const WtDesiredChunk *desired = nullptr;
			for (const WtDesiredChunk &candidate : *desired_chunks) {
				if (candidate.key == key) {
					desired = &candidate;
					break;
				}
			}
			if (desired == nullptr) {
				++metrics_.state_rejections;
				return WtEditRuntimeReplacementStatus::RuntimeStateMismatch;
			}
			collision_required = desired->collision_required;
			visual_required = desired->visual_required;
		}
		const bool owned_cells_intersect = intersects_owned_cells(key, transaction);
		const bool active_visual = active_visual_chunks == nullptr ||
			std::binary_search(
				active_visual_chunks->begin(), active_visual_chunks->end(), key
			);
		prepared_.push_back({
			key,
			record->generation,
			record->source_revision,
			record->world_revision,
			record->priority,
			collision_required,
			visual_required,
			key.lod == 0 && contains_command_center(key, transaction),
			active_visual && owned_cells_intersect,
			visual_required && owned_cells_intersect && !active_visual,
			transaction_delta(key, transaction),
		});
	}
	std::sort(
		prepared_.begin(),
		prepared_.end(),
		[](const PreparedReplacement &left,
			const PreparedReplacement &right) {
			if (left.foreground_interaction != right.foreground_interaction) {
				return left.foreground_interaction;
			}
			if (left.collision_required != right.collision_required) {
				return left.collision_required;
			}
			return left.key < right.key;
		}
	);
	if (scheduler.available_job_capacity() < prepared_.size()) {
		++metrics_.capacity_rejections;
		return WtEditRuntimeReplacementStatus::JobQueueCapacityExceeded;
	}
	prepared_source_revision_ = transaction.source_revision;
	prepared_base_revision_ = transaction.base_revision;
	prepared_committed_revision_ = transaction.committed_revision;
	has_prepared_ = true;
	return WtEditRuntimeReplacementStatus::Ok;
}

WtEditRuntimeReplacementStatus
WtEditRuntimeReplacementService::apply_prepared(
	const WtEditTransaction &transaction,
	WtStreamScheduler &scheduler,
	WtStoragePageCache &page_cache,
	WtChunkResourceCache &resource_cache,
	WtChunkApplicationService &application,
	WtPageMeshingRuntimeOwner *page_meshing_runtime
) {
	(void)page_cache;
	if (!has_prepared_ ||
		transaction.source_revision != prepared_source_revision_ ||
		transaction.base_revision != prepared_base_revision_ ||
		transaction.committed_revision != prepared_committed_revision_) {
		++metrics_.state_rejections;
		return WtEditRuntimeReplacementStatus::RuntimeStateMismatch;
	}
	has_prepared_ = false;
	if (prepared_.empty()) {
		++metrics_.completed_transactions;
		++metrics_.empty_transactions;
		return WtEditRuntimeReplacementStatus::Ok;
	}
	for (const PreparedReplacement &replacement : prepared_) {
		if (page_meshing_runtime != nullptr) {
			const WtPageMeshingRuntimeOwnerStatus status =
				page_meshing_runtime->cancel_owned_generation(
					replacement.key,
					replacement.previous_generation
				);
			if (status == WtPageMeshingRuntimeOwnerStatus::Ok) {
				++metrics_.cancelled_page_meshing_generations;
			} else if (status != WtPageMeshingRuntimeOwnerStatus::NotFound &&
				status != WtPageMeshingRuntimeOwnerStatus::StaleGeneration) {
				++metrics_.page_meshing_runtime_failures;
				return WtEditRuntimeReplacementStatus::PageMeshingRuntimeFailure;
			}
		}
		const WtSchedulerStatus scheduler_status =
			scheduler.request_edited_chunk_version(
				replacement.key,
				replacement.source_revision,
				transaction.committed_revision,
				replacement.collision_required ||
					replacement.foreground_interaction ?
					kWtInteractiveEditPriority : replacement.priority,
				replacement.edit_delta
			);
		if (scheduler_status != WtSchedulerStatus::Ok) {
			++metrics_.scheduler_failures;
			return WtEditRuntimeReplacementStatus::SchedulerFailure;
		}
		const WtChunkRecord *current = scheduler.find_record(replacement.key);
		if (current == nullptr) {
			++metrics_.scheduler_failures;
			return WtEditRuntimeReplacementStatus::SchedulerFailure;
		}
		if (application.expect_chunk(
				replacement.key,
				current->generation,
				replacement.collision_required,
				replacement.visual_required,
				true,
				replacement.collision_required,
				current->world_revision,
				replacement.independently_publishable
			) != WtApplicationStatus::Ok) {
			++metrics_.application_failures;
			return WtEditRuntimeReplacementStatus::ApplicationFailure;
		}

		// Storage pages are immutable source data, keyed by chunk and source
		// revision. The committed journal is replayed over that base page while
		// preparing the replacement generation, so evicting it here only forces
		// an unnecessary storage request on the edit critical path.
		const std::size_t page_entries = 0;
		const std::size_t resource_entries =
			replacement.collision_required ?
				resource_cache.erase_visual_key(replacement.key) :
				resource_cache.erase_key(replacement.key);
		last_replacements_.push_back({
			replacement.key,
			replacement.previous_generation,
			current->generation,
			replacement.source_revision,
			replacement.previous_world_revision,
			transaction.committed_revision,
			page_entries,
			resource_entries,
			replacement.collision_required,
			replacement.visual_required,
			replacement.independently_publishable,
		});
		metrics_.evicted_page_entries += page_entries;
		metrics_.evicted_resource_entries += resource_entries;
		++metrics_.exact_delta_chunks;
		const std::uint64_t dirty_blocks = count_dirty_blocks(
			replacement.edit_delta.dirty_regular_brick_mask
		);
		metrics_.exact_delta_dirty_blocks += dirty_blocks;
		metrics_.maximum_dirty_blocks_per_chunk = std::max(
			metrics_.maximum_dirty_blocks_per_chunk, dirty_blocks
		);
		if (replacement.independently_publishable) {
			++metrics_.active_visual_cohort_chunks;
		}
		if (replacement.deferred_inactive_visual) {
			++metrics_.deferred_inactive_visual_chunks;
		}
	}

	++metrics_.completed_transactions;
	metrics_.replaced_chunks += last_replacements_.size();
	return WtEditRuntimeReplacementStatus::Ok;
}

WtEditRuntimeReplacementStatus
WtEditRuntimeReplacementService::replace_loaded_chunks(
	const WtEditTransaction &transaction,
	const WtEditSpatialIndex &spatial_index,
	WtStreamScheduler &scheduler,
	WtStoragePageCache &page_cache,
	WtChunkResourceCache &resource_cache,
	WtChunkApplicationService &application,
	WtPageMeshingRuntimeOwner *page_meshing_runtime,
	const std::vector<WtDesiredChunk> *desired_chunks,
	const std::vector<WtChunkKey> *active_visual_chunks
) {
	const WtEditRuntimeReplacementStatus prepare = prepare_loaded_chunks(
		transaction, spatial_index, scheduler, application, desired_chunks,
		active_visual_chunks
	);
	if (prepare != WtEditRuntimeReplacementStatus::Ok) return prepare;
	return apply_prepared(
		transaction,
		scheduler,
		page_cache,
		resource_cache,
		application,
		page_meshing_runtime
	);
}

std::size_t WtEditRuntimeReplacementService::replacement_capacity() const noexcept {
	return replacement_capacity_;
}

const std::vector<WtEditRuntimeReplacementRecord> &
WtEditRuntimeReplacementService::get_last_replacements() const noexcept {
	return last_replacements_;
}

WtEditRuntimeReplacementMetrics
WtEditRuntimeReplacementService::get_metrics() const noexcept {
	return metrics_;
}

} // namespace world_transvoxel
