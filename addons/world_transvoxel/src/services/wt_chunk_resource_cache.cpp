#include "services/wt_chunk_resource_cache.h"

#include "services/wt_chunk_resource_payload.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace world_transvoxel {
namespace {

template <typename Entry>
bool identity_less(
	const Entry &entry,
	const std::pair<WtChunkKey, WtGenerationToken> &identity
) noexcept {
	if (entry.key != identity.first) {
		return entry.key < identity.first;
	}
	return entry.generation.value < identity.second.value;
}

template <typename Entry>
bool eviction_precedes(const Entry &left, const Entry &right) noexcept {
	return std::tie(
		left.last_access,
		left.key.x,
		left.key.y,
		left.key.z,
		left.key.lod,
		left.generation.value
	) < std::tie(
		right.last_access,
		right.key.x,
		right.key.y,
		right.key.z,
		right.key.lod,
		right.generation.value
	);
}

} // namespace

WtChunkResourceCache::WtChunkResourceCache(WtChunkResourceCacheLimits limits) :
		limits_(limits) {
	valid_ =
		limits_.mesh_entry_capacity != 0 &&
		limits_.mesh_entry_capacity <= kWtMaximumResourceCacheEntries &&
		limits_.mesh_byte_capacity >= sizeof(WtChunkMeshResult) &&
		limits_.mesh_byte_capacity <= kWtMaximumResourceCacheBytes &&
		limits_.render_entry_capacity != 0 &&
		limits_.render_entry_capacity <= kWtMaximumResourceCacheEntries &&
		limits_.render_byte_capacity >= sizeof(WtRenderPayload) &&
		limits_.render_byte_capacity <= kWtMaximumResourceCacheBytes &&
		limits_.collision_entry_capacity != 0 &&
		limits_.collision_entry_capacity <= kWtMaximumResourceCacheEntries &&
		limits_.collision_byte_capacity >= sizeof(WtCollisionPayload) &&
		limits_.collision_byte_capacity <= kWtMaximumResourceCacheBytes;
	if (valid_) {
		meshes_.reserve(limits_.mesh_entry_capacity);
		renders_.reserve(limits_.render_entry_capacity);
		collisions_.reserve(limits_.collision_entry_capacity);
	}
}

bool WtChunkResourceCache::valid() const noexcept {
	return valid_;
}

WtChunkResourceCacheStatus WtChunkResourceCache::insert_mesh(
	std::shared_ptr<const WtChunkMeshResult> mesh,
	WtGenerationToken generation,
	WtGenerationToken current_generation
) {
	if (!valid_) return WtChunkResourceCacheStatus::InvalidConfiguration;
	if (!mesh || generation.value == 0 || current_generation.value == 0) {
		return WtChunkResourceCacheStatus::InvalidInput;
	}
	if (generation != current_generation) {
		++metrics_.stale_rejections;
		return WtChunkResourceCacheStatus::StaleGeneration;
	}
	if (!wt_is_valid_mesh_payload(*mesh)) {
		++metrics_.invalid_payloads;
		return WtChunkResourceCacheStatus::InvalidPayload;
	}
	const std::size_t resident_bytes = wt_mesh_payload_resident_bytes(*mesh);
	if (resident_bytes > limits_.mesh_byte_capacity) {
		++metrics_.mesh.oversize_rejections;
		return WtChunkResourceCacheStatus::MeshItemTooLarge;
	}
	auto existing = find_mesh_entry(mesh->key, generation);
	if (existing != meshes_.end()) {
		if (!wt_equal_mesh_payload(*existing->payload, *mesh)) {
			++metrics_.identity_conflicts;
			return WtChunkResourceCacheStatus::IdentityConflict;
		}
		mesh_resident_bytes_ -= existing->resident_bytes;
		existing->payload = std::move(mesh);
		existing->resident_bytes = resident_bytes;
		existing->last_access = next_access();
		mesh_resident_bytes_ += resident_bytes;
		++metrics_.mesh.refreshes;
		evict_mesh_to_limits();
		return WtChunkResourceCacheStatus::Ok;
	}
	for (auto iterator = meshes_.begin(); iterator != meshes_.end();) {
		if (iterator->key == mesh->key) {
			mesh_resident_bytes_ -= iterator->resident_bytes;
			iterator = meshes_.erase(iterator);
			++metrics_.mesh.superseded;
		} else {
			++iterator;
		}
	}
	MeshEntry entry;
	entry.key = mesh->key;
	entry.generation = generation;
	entry.payload = std::move(mesh);
	entry.resident_bytes = resident_bytes;
	entry.last_access = next_access();
	const auto position = std::lower_bound(
		meshes_.begin(),
		meshes_.end(),
		std::make_pair(entry.key, entry.generation),
		identity_less<MeshEntry>
	);
	mesh_resident_bytes_ += resident_bytes;
	meshes_.insert(position, std::move(entry));
	++metrics_.mesh.insertions;
	evict_mesh_to_limits();
	return WtChunkResourceCacheStatus::Ok;
}

WtChunkResourceCacheStatus WtChunkResourceCache::insert_render(
	std::shared_ptr<const WtRenderPayload> render,
	WtGenerationToken current_generation
) {
	if (!valid_) return WtChunkResourceCacheStatus::InvalidConfiguration;
	if (!render || current_generation.value == 0) {
		return WtChunkResourceCacheStatus::InvalidInput;
	}
	if (render->generation != current_generation) {
		++metrics_.stale_rejections;
		return WtChunkResourceCacheStatus::StaleGeneration;
	}
	if (!wt_is_valid_render_payload(*render)) {
		++metrics_.invalid_payloads;
		return WtChunkResourceCacheStatus::InvalidPayload;
	}
	const std::size_t resident_bytes =
		wt_render_payload_resident_bytes(*render);
	if (resident_bytes > limits_.render_byte_capacity) {
		++metrics_.render.oversize_rejections;
		return WtChunkResourceCacheStatus::RenderItemTooLarge;
	}
	auto existing = find_render_entry(render->key, render->generation);
	if (existing != renders_.end()) {
		if (!wt_equal_render_payload(*existing->payload, *render)) {
			++metrics_.identity_conflicts;
			return WtChunkResourceCacheStatus::IdentityConflict;
		}
		render_resident_bytes_ -= existing->resident_bytes;
		existing->payload = std::move(render);
		existing->resident_bytes = resident_bytes;
		existing->last_access = next_access();
		render_resident_bytes_ += resident_bytes;
		++metrics_.render.refreshes;
		evict_render_to_limits();
		return WtChunkResourceCacheStatus::Ok;
	}
	for (auto iterator = renders_.begin(); iterator != renders_.end();) {
		if (iterator->key == render->key) {
			render_resident_bytes_ -= iterator->resident_bytes;
			iterator = renders_.erase(iterator);
			++metrics_.render.superseded;
		} else {
			++iterator;
		}
	}
	RenderEntry entry;
	entry.key = render->key;
	entry.generation = render->generation;
	entry.payload = std::move(render);
	entry.resident_bytes = resident_bytes;
	entry.last_access = next_access();
	const auto position = std::lower_bound(
		renders_.begin(),
		renders_.end(),
		std::make_pair(entry.key, entry.generation),
		identity_less<RenderEntry>
	);
	render_resident_bytes_ += resident_bytes;
	renders_.insert(position, std::move(entry));
	++metrics_.render.insertions;
	evict_render_to_limits();
	return WtChunkResourceCacheStatus::Ok;
}

WtChunkResourceCacheStatus WtChunkResourceCache::insert_collision(
	std::shared_ptr<const WtCollisionPayload> collision,
	WtGenerationToken current_generation
) {
	if (!valid_) return WtChunkResourceCacheStatus::InvalidConfiguration;
	if (!collision || current_generation.value == 0) {
		return WtChunkResourceCacheStatus::InvalidInput;
	}
	if (collision->generation != current_generation) {
		++metrics_.stale_rejections;
		return WtChunkResourceCacheStatus::StaleGeneration;
	}
	if (!wt_is_valid_collision_payload(*collision)) {
		++metrics_.invalid_payloads;
		return WtChunkResourceCacheStatus::InvalidPayload;
	}
	const std::size_t resident_bytes =
		wt_collision_payload_resident_bytes(*collision);
	if (resident_bytes > limits_.collision_byte_capacity) {
		++metrics_.collision.oversize_rejections;
		return WtChunkResourceCacheStatus::CollisionItemTooLarge;
	}
	auto existing = find_collision_entry(collision->key, collision->generation);
	if (existing != collisions_.end()) {
		if (!wt_equal_collision_payload(*existing->payload, *collision)) {
			++metrics_.identity_conflicts;
			return WtChunkResourceCacheStatus::IdentityConflict;
		}
		collision_resident_bytes_ -= existing->resident_bytes;
		existing->payload = std::move(collision);
		existing->resident_bytes = resident_bytes;
		existing->last_access = next_access();
		collision_resident_bytes_ += resident_bytes;
		++metrics_.collision.refreshes;
		evict_collision_to_limits();
		return WtChunkResourceCacheStatus::Ok;
	}
	for (auto iterator = collisions_.begin(); iterator != collisions_.end();) {
		if (iterator->key == collision->key) {
			collision_resident_bytes_ -= iterator->resident_bytes;
			iterator = collisions_.erase(iterator);
			++metrics_.collision.superseded;
		} else {
			++iterator;
		}
	}
	CollisionEntry entry;
	entry.key = collision->key;
	entry.generation = collision->generation;
	entry.payload = std::move(collision);
	entry.resident_bytes = resident_bytes;
	entry.last_access = next_access();
	const auto position = std::lower_bound(
		collisions_.begin(),
		collisions_.end(),
		std::make_pair(entry.key, entry.generation),
		identity_less<CollisionEntry>
	);
	collision_resident_bytes_ += resident_bytes;
	collisions_.insert(position, std::move(entry));
	++metrics_.collision.insertions;
	evict_collision_to_limits();
	return WtChunkResourceCacheStatus::Ok;
}

std::shared_ptr<const WtChunkMeshResult> WtChunkResourceCache::find_mesh(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	auto entry = find_mesh_entry(key, generation);
	if (entry == meshes_.end()) {
		++metrics_.mesh.misses;
		return {};
	}
	entry->last_access = next_access();
	++metrics_.mesh.hits;
	return entry->payload;
}

std::shared_ptr<const WtRenderPayload> WtChunkResourceCache::find_render(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	auto entry = find_render_entry(key, generation);
	if (entry == renders_.end()) {
		++metrics_.render.misses;
		return {};
	}
	entry->last_access = next_access();
	++metrics_.render.hits;
	return entry->payload;
}

std::shared_ptr<const WtCollisionPayload>
WtChunkResourceCache::find_collision(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	auto entry = find_collision_entry(key, generation);
	if (entry == collisions_.end()) {
		++metrics_.collision.misses;
		return {};
	}
	entry->last_access = next_access();
	++metrics_.collision.hits;
	return entry->payload;
}

std::size_t WtChunkResourceCache::erase_key(const WtChunkKey &key) {
	std::size_t erased = 0;
	for (auto iterator = meshes_.begin(); iterator != meshes_.end();) {
		if (iterator->key == key) {
			mesh_resident_bytes_ -= iterator->resident_bytes;
			iterator = meshes_.erase(iterator);
			++erased;
		} else ++iterator;
	}
	for (auto iterator = renders_.begin(); iterator != renders_.end();) {
		if (iterator->key == key) {
			render_resident_bytes_ -= iterator->resident_bytes;
			iterator = renders_.erase(iterator);
			++erased;
		} else ++iterator;
	}
	for (auto iterator = collisions_.begin(); iterator != collisions_.end();) {
		if (iterator->key == key) {
			collision_resident_bytes_ -= iterator->resident_bytes;
			iterator = collisions_.erase(iterator);
			++erased;
		} else ++iterator;
	}
	return erased;
}

void WtChunkResourceCache::clear() noexcept {
	meshes_.clear();
	renders_.clear();
	collisions_.clear();
	mesh_resident_bytes_ = 0;
	render_resident_bytes_ = 0;
	collision_resident_bytes_ = 0;
}

std::uint64_t WtChunkResourceCache::next_access() noexcept {
	if (access_counter_ == std::numeric_limits<std::uint64_t>::max()) {
		std::uint64_t minimum = access_counter_;
		for (const MeshEntry &entry : meshes_) {
			minimum = std::min(minimum, entry.last_access);
		}
		for (const RenderEntry &entry : renders_) {
			minimum = std::min(minimum, entry.last_access);
		}
		for (const CollisionEntry &entry : collisions_) {
			minimum = std::min(minimum, entry.last_access);
		}
		if (meshes_.empty() && renders_.empty() && collisions_.empty()) {
			access_counter_ = 0;
		} else {
			const std::uint64_t offset = minimum - 1;
			for (MeshEntry &entry : meshes_) entry.last_access -= offset;
			for (RenderEntry &entry : renders_) entry.last_access -= offset;
			for (CollisionEntry &entry : collisions_) {
				entry.last_access -= offset;
			}
			access_counter_ -= offset;
		}
	}
	return ++access_counter_;
}

std::vector<WtChunkResourceCache::MeshEntry>::iterator
WtChunkResourceCache::find_mesh_entry(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	const auto identity = std::make_pair(key, generation);
	const auto iterator = std::lower_bound(
		meshes_.begin(), meshes_.end(), identity, identity_less<MeshEntry>
	);
	return iterator != meshes_.end() &&
		iterator->key == key && iterator->generation == generation ?
		iterator : meshes_.end();
}

std::vector<WtChunkResourceCache::RenderEntry>::iterator
WtChunkResourceCache::find_render_entry(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	const auto identity = std::make_pair(key, generation);
	const auto iterator = std::lower_bound(
		renders_.begin(), renders_.end(), identity, identity_less<RenderEntry>
	);
	return iterator != renders_.end() &&
		iterator->key == key && iterator->generation == generation ?
		iterator : renders_.end();
}

std::vector<WtChunkResourceCache::CollisionEntry>::iterator
WtChunkResourceCache::find_collision_entry(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	const auto identity = std::make_pair(key, generation);
	const auto iterator = std::lower_bound(
		collisions_.begin(),
		collisions_.end(),
		identity,
		identity_less<CollisionEntry>
	);
	return iterator != collisions_.end() &&
		iterator->key == key && iterator->generation == generation ?
		iterator : collisions_.end();
}

void WtChunkResourceCache::evict_mesh_to_limits() {
	while (meshes_.size() > limits_.mesh_entry_capacity ||
		mesh_resident_bytes_ > limits_.mesh_byte_capacity) {
		const auto victim = std::min_element(
			meshes_.begin(), meshes_.end(), eviction_precedes<MeshEntry>
		);
		mesh_resident_bytes_ -= victim->resident_bytes;
		meshes_.erase(victim);
		++metrics_.mesh.evictions;
	}
}

void WtChunkResourceCache::evict_render_to_limits() {
	while (renders_.size() > limits_.render_entry_capacity ||
		render_resident_bytes_ > limits_.render_byte_capacity) {
		const auto victim = std::min_element(
			renders_.begin(), renders_.end(), eviction_precedes<RenderEntry>
		);
		render_resident_bytes_ -= victim->resident_bytes;
		renders_.erase(victim);
		++metrics_.render.evictions;
	}
}

void WtChunkResourceCache::evict_collision_to_limits() {
	while (collisions_.size() > limits_.collision_entry_capacity ||
		collision_resident_bytes_ > limits_.collision_byte_capacity) {
		const auto victim = std::min_element(
			collisions_.begin(),
			collisions_.end(),
			eviction_precedes<CollisionEntry>
		);
		collision_resident_bytes_ -= victim->resident_bytes;
		collisions_.erase(victim);
		++metrics_.collision.evictions;
	}
}

std::size_t WtChunkResourceCache::mesh_entry_count() const noexcept {
	return meshes_.size();
}

std::size_t WtChunkResourceCache::mesh_resident_bytes() const noexcept {
	return mesh_resident_bytes_;
}

std::size_t WtChunkResourceCache::render_entry_count() const noexcept {
	return renders_.size();
}

std::size_t WtChunkResourceCache::render_resident_bytes() const noexcept {
	return render_resident_bytes_;
}

std::size_t WtChunkResourceCache::collision_entry_count() const noexcept {
	return collisions_.size();
}

std::size_t WtChunkResourceCache::collision_resident_bytes() const noexcept {
	return collision_resident_bytes_;
}

WtChunkResourceCacheMetrics WtChunkResourceCache::get_metrics() const noexcept {
	return metrics_;
}

} // namespace world_transvoxel
