#include "services/wt_desired_set_runtime.h"

#include "services/wt_chunk_application.h"
#include "services/wt_chunk_resource_cache.h"
#include "services/wt_page_meshing_runtime_owner.h"
#include "storage/wt_storage_page_cache.h"
#include "streaming/wt_stream_scheduler.h"

#include <algorithm>

namespace world_transvoxel {
namespace {

constexpr std::size_t kWtDormantChunkCapacity = 64;

template <typename Item, typename KeyFunction>
bool canonical_keys(
	const std::vector<Item> &items,
	KeyFunction key_function
) noexcept {
	for (std::size_t index = 0; index < items.size(); ++index) {
		const WtChunkKey key = key_function(items[index]);
		if (!wt_is_valid_chunk_key(key) ||
			(index != 0 && !(key_function(items[index - 1]) < key))) {
			return false;
		}
	}
	return true;
}

bool contains_desired(
	const std::vector<WtDesiredChunk> &items,
	const WtChunkKey &key
) noexcept {
	const auto iterator = std::lower_bound(
		items.begin(),
		items.end(),
		key,
		[](const WtDesiredChunk &item, const WtChunkKey &value) {
			return item.key < value;
		}
	);
	return iterator != items.end() && iterator->key == key;
}

bool contains_key(
	const std::vector<WtChunkKey> &items,
	const WtChunkKey &key
) noexcept {
	return std::binary_search(items.begin(), items.end(), key);
}

bool role_promotion_requires_remesh(
	WtChunkLifecycle lifecycle,
	const WtChunkKey &key,
	WtGenerationToken generation,
	const WtPageMeshingRuntimeOwner *page_meshing_runtime,
	bool collision_promotion
) noexcept {
	if (lifecycle != WtChunkLifecycle::Meshing &&
		lifecycle != WtChunkLifecycle::Ready) return false;
	return page_meshing_runtime == nullptr ||
		!page_meshing_runtime->owned_generation_accepts_role_promotion(
			key, generation, collision_promotion
		);
}

} // namespace

WtDesiredSetRuntimeService::WtDesiredSetRuntimeService(
	std::size_t change_capacity
) :
		change_capacity_(change_capacity),
		dormant_capacity_(std::min(change_capacity, kWtDormantChunkCapacity)),
		valid_(change_capacity > 0 &&
			change_capacity <= kWtMaximumDesiredChunkCount) {
	if (valid_) dormant_chunks_.reserve(dormant_capacity_);
}

bool WtDesiredSetRuntimeService::valid() const noexcept {
	return valid_;
}

bool WtDesiredSetRuntimeService::copy_dormant(
	const WtChunkKey &key,
	DormantChunk &output
) const noexcept {
	std::lock_guard<std::mutex> lock(dormant_mutex_);
	const auto found = std::find_if(
		dormant_chunks_.begin(), dormant_chunks_.end(),
		[&key](const DormantChunk &item) { return item.key == key; }
	);
	if (found == dormant_chunks_.end()) return false;
	output = *found;
	return true;
}

void WtDesiredSetRuntimeService::erase_dormant(
	const WtChunkKey &key
) noexcept {
	std::lock_guard<std::mutex> lock(dormant_mutex_);
	dormant_chunks_.erase(std::remove_if(
		dormant_chunks_.begin(), dormant_chunks_.end(),
		[&key](const DormantChunk &item) { return item.key == key; }
	), dormant_chunks_.end());
	metrics_.dormant_entries = dormant_chunks_.size();
}

void WtDesiredSetRuntimeService::retain_dormant(
	const WtChunkRecord &record
) noexcept {
	std::lock_guard<std::mutex> lock(dormant_mutex_);
	dormant_chunks_.erase(std::remove_if(
		dormant_chunks_.begin(), dormant_chunks_.end(),
		[&record](const DormantChunk &item) { return item.key == record.key; }
	), dormant_chunks_.end());
	dormant_chunks_.push_back({
		record.key, record.generation, record.source_revision, record.world_revision,
	});
	++metrics_.dormant_insertions;
	metrics_.dormant_entries = dormant_chunks_.size();
	metrics_.dormant_entry_peak = std::max(
		metrics_.dormant_entry_peak,
		static_cast<std::uint64_t>(dormant_chunks_.size())
	);
}

bool WtDesiredSetRuntimeService::has_dormant_generation(
	const WtChunkKey &key,
	WtGenerationToken generation
) const noexcept {
	DormantChunk dormant;
	return copy_dormant(key, dormant) && dormant.generation == generation;
}

bool WtDesiredSetRuntimeService::validate_delta(
	const WtDesiredSetDelta &delta
) const noexcept {
	if (!canonical_keys(
			delta.added,
			[](const WtDesiredChunk &item) { return item.key; }
		) ||
		!canonical_keys(
			delta.removed,
			[](const WtChunkKey &item) { return item; }
		) ||
		!canonical_keys(
			delta.updated,
			[](const WtDesiredChunk &item) { return item.key; }
		)) {
		return false;
	}
	for (const WtDesiredChunk &item : delta.added) {
		if (item.supporter_count == 0 ||
			(!item.visual_required && !item.collision_required) ||
			contains_key(delta.removed, item.key) ||
			contains_desired(delta.updated, item.key)) {
			return false;
		}
	}
	for (const WtDesiredChunk &item : delta.updated) {
		if (item.supporter_count == 0 ||
			(!item.visual_required && !item.collision_required) ||
			contains_key(delta.removed, item.key)) {
			return false;
		}
	}
	return true;
}

WtDesiredSetRuntimeStatus WtDesiredSetRuntimeService::apply_delta(
	const WtDesiredSetDelta &delta,
	std::uint64_t source_revision,
	std::uint64_t world_revision,
	WtStreamScheduler &scheduler,
	WtStoragePageCache &page_cache,
	WtChunkResourceCache &resource_cache,
	WtChunkApplicationService &application,
	WtPageMeshingRuntimeOwner *page_meshing_runtime
) {
	++metrics_.delta_attempts;
	if (!valid_) {
		++metrics_.capacity_rejections;
		return WtDesiredSetRuntimeStatus::InvalidConfiguration;
	}
	if (delta.added.size() > change_capacity_ ||
		delta.removed.size() > change_capacity_ - delta.added.size() ||
		delta.updated.size() >
			change_capacity_ - delta.added.size() - delta.removed.size()) {
		++metrics_.capacity_rejections;
		return WtDesiredSetRuntimeStatus::ChangeCapacityExceeded;
	}
	if (!validate_delta(delta)) {
		++metrics_.invalid_deltas;
		return WtDesiredSetRuntimeStatus::InvalidDelta;
	}
	if (delta.added.empty() && delta.removed.empty() && delta.updated.empty()) {
		++metrics_.applied_deltas;
		++metrics_.empty_deltas;
		return WtDesiredSetRuntimeStatus::Ok;
	}

	for (const WtChunkKey &key : delta.removed) {
		const WtChunkRecord *record = scheduler.find_record(key);
		WtChunkApplicationRecord application_record;
		const bool application_present =
			application.copy_record(key, application_record);
		if (record == nullptr ||
			(application_present &&
				record->generation != application_record.generation) ||
			(!application_present &&
				record->lifecycle != WtChunkLifecycle::Ready)) {
			++metrics_.state_rejections;
			return WtDesiredSetRuntimeStatus::RuntimeStateMismatch;
		}
	}
	for (const WtDesiredChunk &item : delta.updated) {
		const WtChunkRecord *record = scheduler.find_record(item.key);
		WtChunkApplicationRecord application_record;
		if (record == nullptr ||
			!application.copy_record(item.key, application_record) ||
			record->generation != application_record.generation) {
			++metrics_.state_rejections;
			return WtDesiredSetRuntimeStatus::RuntimeStateMismatch;
		}
	}
	for (const WtDesiredChunk &item : delta.added) {
		WtChunkApplicationRecord application_record;
		const WtChunkRecord *record = scheduler.find_record(item.key);
		DormantChunk dormant;
		const bool dormant_match = copy_dormant(item.key, dormant) &&
			record != nullptr && record->generation == dormant.generation;
		if ((record != nullptr && !dormant_match) ||
			application.copy_record(item.key, application_record)) {
			++metrics_.state_rejections;
			return WtDesiredSetRuntimeStatus::RuntimeStateMismatch;
		}
	}

	std::vector<WtChunkKey> reusable_additions;
	std::vector<WtChunkKey> dormant_evictions;
	for (const WtDesiredChunk &item : delta.added) {
		DormantChunk dormant;
		const WtChunkRecord *record = scheduler.find_record(item.key);
		if (!copy_dormant(item.key, dormant)) continue;
		const bool visual_cached = !item.visual_required || static_cast<bool>(
			resource_cache.find_render(item.key, dormant.generation)
		);
		const bool collision_cached = !item.collision_required || static_cast<bool>(
			resource_cache.find_collision(item.key, dormant.generation)
		);
		if (record != nullptr && record->generation == dormant.generation &&
				record->source_revision == source_revision &&
				record->world_revision == world_revision &&
				record->lifecycle == WtChunkLifecycle::Ready &&
				visual_cached && collision_cached) {
			reusable_additions.push_back(item.key);
		} else {
			dormant_evictions.push_back(item.key);
		}
	}
	const std::size_t stale_dormant_eviction_count = dormant_evictions.size();
	const std::size_t new_addition_count =
		delta.added.size() - reusable_additions.size();
	std::size_t record_shortfall = new_addition_count >
			scheduler.available_record_capacity() +
			dormant_evictions.size() + delta.removed.size() ?
		new_addition_count - (
			scheduler.available_record_capacity() +
			dormant_evictions.size() + delta.removed.size()
		) : 0;
	if (record_shortfall != 0) {
		std::lock_guard<std::mutex> lock(dormant_mutex_);
		for (const DormantChunk &dormant : dormant_chunks_) {
			if (record_shortfall == 0) break;
			if (std::find(
					reusable_additions.begin(), reusable_additions.end(), dormant.key
				) != reusable_additions.end() || std::find(
					dormant_evictions.begin(), dormant_evictions.end(), dormant.key
				) != dormant_evictions.end()) {
				continue;
			}
			dormant_evictions.push_back(dormant.key);
			--record_shortfall;
		}
	}
	std::size_t effective_record_capacity =
		scheduler.available_record_capacity() + dormant_evictions.size();
	if (new_addition_count > effective_record_capacity + delta.removed.size() ||
		delta.added.size() >
			application.available_record_capacity() + delta.removed.size()) {
		++metrics_.capacity_rejections;
		return WtDesiredSetRuntimeStatus::RecordCapacityExceeded;
	}

	std::vector<WtChunkKey> retain_removals;
	std::size_t dormant_count = 0;
	{
		std::lock_guard<std::mutex> lock(dormant_mutex_);
		dormant_count = dormant_chunks_.size();
	}
	const std::size_t dormant_after_additions =
		dormant_count - reusable_additions.size() - dormant_evictions.size();
	const std::size_t dormant_slots = dormant_capacity_ > dormant_after_additions ?
		dormant_capacity_ - dormant_after_additions : 0;
	const std::size_t record_slots =
		effective_record_capacity + delta.removed.size() - new_addition_count;
	const std::size_t retain_capacity = std::min(dormant_slots, record_slots);
	for (const WtChunkKey &key : delta.removed) {
		if (retain_removals.size() >= retain_capacity) break;
		const WtChunkRecord *record = scheduler.find_record(key);
		if (record != nullptr && record->lifecycle == WtChunkLifecycle::Ready) {
			retain_removals.push_back(key);
		}
	}
	std::size_t role_promotions_requiring_remesh = 0;
	for (const WtDesiredChunk &item : delta.updated) {
		const WtChunkRecord *record = scheduler.find_record(item.key);
		WtChunkApplicationRecord application_record;
		if (record != nullptr &&
			application.copy_record(item.key, application_record) &&
			((!application_record.visual_required && item.visual_required) ||
			(!application_record.collision_required && item.collision_required)) &&
			role_promotion_requires_remesh(
				record->lifecycle,
				item.key,
				record->generation,
				page_meshing_runtime,
				!application_record.collision_required && item.collision_required
			) && !(!application_record.collision_required &&
				item.collision_required && application_record.visual_required)) {
			++role_promotions_requiring_remesh;
		}
	}
	if (scheduler.available_job_capacity() <
			new_addition_count + role_promotions_requiring_remesh) {
		++metrics_.capacity_rejections;
		return WtDesiredSetRuntimeStatus::JobQueueCapacityExceeded;
	}
	std::vector<WtChunkPriorityUpdate> scheduler_priority_updates;
	scheduler_priority_updates.reserve(delta.updated.size());
	for (std::size_t eviction_index = 0;
			eviction_index < dormant_evictions.size(); ++eviction_index) {
		const WtChunkKey &key = dormant_evictions[eviction_index];
		if (scheduler.forget_chunk(key) != WtSchedulerStatus::Ok) {
			++metrics_.scheduler_failures;
			return WtDesiredSetRuntimeStatus::SchedulerFailure;
		}
		erase_dormant(key);
		metrics_.evicted_page_entries += page_cache.erase_key(key);
		metrics_.evicted_resource_entries += resource_cache.erase_key(key);
		++metrics_.dormant_evictions;
		if (eviction_index < stale_dormant_eviction_count) {
			++metrics_.dormant_stale_evictions;
		}
	}

	for (const WtChunkKey &key : delta.removed) {
		if (page_meshing_runtime != nullptr) {
			const WtPageMeshingRuntimeOwnerStatus status =
				page_meshing_runtime->release_owned_chunk(key);
			if (status == WtPageMeshingRuntimeOwnerStatus::Ok) {
				++metrics_.released_page_meshing_records;
			} else if (status != WtPageMeshingRuntimeOwnerStatus::NotFound &&
				status != WtPageMeshingRuntimeOwnerStatus::StaleGeneration) {
				++metrics_.page_meshing_runtime_failures;
				return WtDesiredSetRuntimeStatus::PageMeshingRuntimeFailure;
			}
		}
		const bool retain = std::binary_search(
			retain_removals.begin(), retain_removals.end(), key
		);
		const WtChunkRecord retained_record = *scheduler.find_record(key);
		if (!retain && scheduler.forget_chunk(key) != WtSchedulerStatus::Ok) {
			++metrics_.scheduler_failures;
			return WtDesiredSetRuntimeStatus::SchedulerFailure;
		}
		const WtApplicationStatus forget_status = application.forget_chunk(key);
		if (forget_status != WtApplicationStatus::Ok &&
			forget_status != WtApplicationStatus::NotFound) {
			++metrics_.application_failures;
			return WtDesiredSetRuntimeStatus::ApplicationFailure;
		}
		if (retain) {
			retain_dormant(retained_record);
		} else {
			metrics_.evicted_page_entries += page_cache.erase_key(key);
			metrics_.evicted_resource_entries += resource_cache.erase_key(key);
		}
	}
	for (const WtDesiredChunk &item : delta.updated) {
		WtChunkApplicationRecord application_record;
		const bool copied_application_record =
			application.copy_record(item.key, application_record);
		const bool promote_visual =
			copied_application_record &&
			!application_record.visual_required && item.visual_required;
		const bool promote_collision =
			copied_application_record &&
			!application_record.collision_required && item.collision_required;
		const bool withdraw_collision =
			copied_application_record &&
			application_record.collision_required && !item.collision_required;
		const WtChunkRecord *record = scheduler.find_record(item.key);
		const bool interactive_edit_in_flight =
			record != nullptr &&
			record->priority == kWtInteractiveEditPriority &&
			(record->lifecycle == WtChunkLifecycle::Sampling ||
				record->lifecycle == WtChunkLifecycle::Meshing);
		const std::int32_t effective_priority =
			interactive_edit_in_flight ?
				kWtInteractiveEditPriority : item.priority;
		if ((promote_visual || promote_collision) && record != nullptr &&
			role_promotion_requires_remesh(
				record->lifecycle,
				item.key,
				record->generation,
				page_meshing_runtime,
				promote_collision
			)) {
			const bool collision_only_refresh =
				promote_collision && !promote_visual &&
				application_record.visual_required;
			if (collision_only_refresh) {
				const std::shared_ptr<const WtCollisionPayload> cached_collision =
					resource_cache.find_collision(
						item.key, record->generation
					);
				if (cached_collision) {
					const WtApplicationStatus required_status =
						application.set_collision_required(item.key, true);
					const WtApplicationStatus submit_status =
						application.submit_collision(cached_collision);
					if ((required_status != WtApplicationStatus::Ok &&
						required_status != WtApplicationStatus::AlreadyCurrent) ||
						submit_status != WtApplicationStatus::Ok) {
						++metrics_.application_failures;
						return WtDesiredSetRuntimeStatus::ApplicationFailure;
					}
					continue;
				}
			}
			if (collision_only_refresh &&
				(record->lifecycle != WtChunkLifecycle::Ready ||
					scheduler.available_job_capacity() == 0)) {
				const WtApplicationStatus deferred_status =
					application.begin_collision_only_refresh(
						item.key, record->generation
					);
				if (deferred_status != WtApplicationStatus::Ok &&
					deferred_status != WtApplicationStatus::AlreadyCurrent) {
					++metrics_.application_failures;
					return WtDesiredSetRuntimeStatus::ApplicationFailure;
				}
				continue;
			}
			if (page_meshing_runtime != nullptr) {
				const WtPageMeshingRuntimeOwnerStatus release_status =
					page_meshing_runtime->release_owned_chunk(item.key);
				if (release_status != WtPageMeshingRuntimeOwnerStatus::Ok &&
					release_status != WtPageMeshingRuntimeOwnerStatus::NotFound &&
					release_status !=
						WtPageMeshingRuntimeOwnerStatus::StaleGeneration) {
					++metrics_.page_meshing_runtime_failures;
					return WtDesiredSetRuntimeStatus::PageMeshingRuntimeFailure;
				}
			}
			if (collision_only_refresh) {
				const WtSchedulerStatus refresh_status =
					scheduler.request_same_generation_refresh(
						item.key, record->source_revision,
						record->world_revision,
						effective_priority
					);
				if (refresh_status != WtSchedulerStatus::Ok) {
					++metrics_.scheduler_failures;
					return WtDesiredSetRuntimeStatus::SchedulerFailure;
				}
				const WtApplicationStatus refresh_application_status =
					application.begin_collision_only_refresh(
						item.key, record->generation
					);
				if (refresh_application_status != WtApplicationStatus::Ok) {
					++metrics_.application_failures;
					return WtDesiredSetRuntimeStatus::ApplicationFailure;
				}
				continue;
			}
			const WtSchedulerStatus remesh_status =
				scheduler.request_chunk_version(
					item.key,
					source_revision,
					world_revision,
					effective_priority,
					true
				);
			if (remesh_status != WtSchedulerStatus::Ok) {
				++metrics_.scheduler_failures;
				return WtDesiredSetRuntimeStatus::SchedulerFailure;
			}
			record = scheduler.find_record(item.key);
			if (record == nullptr || application.expect_chunk(
					item.key,
					record->generation,
					item.collision_required,
					item.visual_required,
					true,
					application_record.collision_ready &&
						item.collision_required,
					record->world_revision
				) != WtApplicationStatus::Ok) {
				++metrics_.application_failures;
				return WtDesiredSetRuntimeStatus::ApplicationFailure;
			}
			if (withdraw_collision) {
				metrics_.evicted_resource_entries +=
					resource_cache.erase_collision_key(item.key);
			}
			continue;
		}
		if (page_meshing_runtime != nullptr && record != nullptr) {
			const WtPageMeshingRuntimeOwnerStatus status =
				page_meshing_runtime->reprioritize_owned_chunk(
					item.key,
					record->generation,
					effective_priority
				);
			if (status == WtPageMeshingRuntimeOwnerStatus::Ok) {
				++metrics_.reprioritized_page_meshing_records;
			} else if (status != WtPageMeshingRuntimeOwnerStatus::NotFound &&
				status != WtPageMeshingRuntimeOwnerStatus::StaleGeneration) {
				++metrics_.page_meshing_runtime_failures;
				return WtDesiredSetRuntimeStatus::PageMeshingRuntimeFailure;
			}
		}
		scheduler_priority_updates.push_back({
			item.key, record->generation, effective_priority,
		});
		// Enable a newly required role before disabling the old one so a chunk
		// never passes through an invalid no-visual/no-collision state.
		WtApplicationStatus application_status = WtApplicationStatus::Ok;
		if (item.visual_required) {
			application_status = application.set_visual_required(
				item.key, true
			);
		}
		if (application_status != WtApplicationStatus::Ok &&
			application_status != WtApplicationStatus::AlreadyCurrent) {
			++metrics_.application_failures;
			return WtDesiredSetRuntimeStatus::ApplicationFailure;
		}
		if (promote_visual) {
			const auto cached_render = resource_cache.find_render(
				item.key, record->generation
			);
			if (cached_render) {
				const WtApplicationStatus submit_status =
					application.submit_render(cached_render);
				if (submit_status != WtApplicationStatus::Ok &&
					submit_status != WtApplicationStatus::AlreadyCurrent) {
					++metrics_.application_failures;
					return WtDesiredSetRuntimeStatus::ApplicationFailure;
				}
			}
		}
		if (item.collision_required) {
			application_status = application.set_collision_required(
				item.key, true
			);
		}
		if (application_status != WtApplicationStatus::Ok &&
			application_status != WtApplicationStatus::AlreadyCurrent) {
			++metrics_.application_failures;
			return WtDesiredSetRuntimeStatus::ApplicationFailure;
		}
		if (!item.visual_required) {
			application_status = application.set_visual_required(
				item.key, false
			);
		}
		if (application_status != WtApplicationStatus::Ok &&
			application_status != WtApplicationStatus::AlreadyCurrent) {
			++metrics_.application_failures;
			return WtDesiredSetRuntimeStatus::ApplicationFailure;
		}
		if (!item.collision_required) {
			application_status = application.set_collision_required(
				item.key, false
			);
		}
		if (application_status != WtApplicationStatus::Ok &&
			application_status != WtApplicationStatus::AlreadyCurrent) {
			++metrics_.application_failures;
			return WtDesiredSetRuntimeStatus::ApplicationFailure;
		}
		if (withdraw_collision) {
			metrics_.evicted_resource_entries +=
				resource_cache.erase_collision_key(item.key);
		}
	}
	if (!scheduler_priority_updates.empty()) {
		const WtSchedulerStatus scheduler_status =
			scheduler.reprioritize_chunks(scheduler_priority_updates);
		if (scheduler_status != WtSchedulerStatus::Ok &&
			scheduler_status != WtSchedulerStatus::AlreadyCurrent) {
			++metrics_.scheduler_failures;
			return WtDesiredSetRuntimeStatus::SchedulerFailure;
		}
	}
	for (const WtDesiredChunk &item : delta.added) {
		if (std::binary_search(
				reusable_additions.begin(), reusable_additions.end(), item.key)) {
			const WtChunkRecord *record = scheduler.find_record(item.key);
			if (record == nullptr || scheduler.reprioritize_chunk(
					item.key, item.priority
				) == WtSchedulerStatus::NotFound ||
					application.expect_chunk(
						item.key,
						record->generation,
						item.collision_required,
						item.visual_required,
						false,
						false,
						record->world_revision
					) != WtApplicationStatus::Ok) {
				++metrics_.application_failures;
				return WtDesiredSetRuntimeStatus::ApplicationFailure;
			}
			if (item.visual_required) {
				const auto cached_render = resource_cache.find_render(
					item.key, record->generation
				);
				if (!cached_render || application.submit_render(cached_render) !=
						WtApplicationStatus::Ok) {
					++metrics_.application_failures;
					return WtDesiredSetRuntimeStatus::ApplicationFailure;
				}
			}
			if (item.collision_required) {
				const auto cached_collision = resource_cache.find_collision(
					item.key, record->generation
				);
				if (!cached_collision || application.submit_collision(
						cached_collision
					) != WtApplicationStatus::Ok) {
					++metrics_.application_failures;
					return WtDesiredSetRuntimeStatus::ApplicationFailure;
				}
			}
			erase_dormant(item.key);
			++metrics_.dormant_reactivations;
			continue;
		}
		if (scheduler.request_chunk_version(
				item.key,
				source_revision,
				world_revision,
				item.priority
			) != WtSchedulerStatus::Ok) {
			++metrics_.scheduler_failures;
			return WtDesiredSetRuntimeStatus::SchedulerFailure;
		}
		const WtChunkRecord *record = scheduler.find_record(item.key);
		if (record == nullptr ||
			application.expect_chunk(
				item.key,
				record->generation,
				item.collision_required,
				item.visual_required,
				false,
				false,
				record->world_revision
			) != WtApplicationStatus::Ok) {
			++metrics_.application_failures;
			return WtDesiredSetRuntimeStatus::ApplicationFailure;
		}
	}

	++metrics_.applied_deltas;
	metrics_.added_chunks += delta.added.size();
	metrics_.removed_chunks += delta.removed.size();
	metrics_.updated_chunks += delta.updated.size();
	return WtDesiredSetRuntimeStatus::Ok;
}

std::size_t WtDesiredSetRuntimeService::change_capacity() const noexcept {
	return change_capacity_;
}

WtDesiredSetRuntimeMetrics
WtDesiredSetRuntimeService::get_metrics() const noexcept {
	return metrics_;
}

} // namespace world_transvoxel
