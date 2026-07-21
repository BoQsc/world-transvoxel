#pragma once

#include "physics/wt_collision_builder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace world_transvoxel {

constexpr std::size_t kWtMaximumResourceCacheEntries = 65536;
constexpr std::size_t kWtMaximumResourceCacheBytes =
	1024U * 1024U * 1024U;

struct WtChunkResourceCacheLimits {
	std::size_t mesh_entry_capacity = 128;
	std::size_t mesh_byte_capacity = 128U * 1024U * 1024U;
	std::size_t render_entry_capacity = 128;
	std::size_t render_byte_capacity = 128U * 1024U * 1024U;
	std::size_t collision_entry_capacity = 64;
	std::size_t collision_byte_capacity = 64U * 1024U * 1024U;
};

enum class WtChunkResourceCacheStatus : std::uint8_t {
	Ok,
	InvalidConfiguration,
	InvalidInput,
	StaleGeneration,
	InvalidPayload,
	IdentityConflict,
	MeshItemTooLarge,
	RenderItemTooLarge,
	CollisionItemTooLarge,
	NotFound,
};

struct WtResourceCacheTierMetrics {
	std::uint64_t hits = 0;
	std::uint64_t misses = 0;
	std::uint64_t insertions = 0;
	std::uint64_t refreshes = 0;
	std::uint64_t evictions = 0;
	std::uint64_t superseded = 0;
	std::uint64_t oversize_rejections = 0;
};

struct WtChunkResourceCacheMetrics {
	std::uint64_t stale_rejections = 0;
	std::uint64_t invalid_payloads = 0;
	std::uint64_t identity_conflicts = 0;
	WtResourceCacheTierMetrics mesh;
	WtResourceCacheTierMetrics render;
	WtResourceCacheTierMetrics collision;
};

class WtChunkResourceCache {
public:
	explicit WtChunkResourceCache(WtChunkResourceCacheLimits limits);

	bool valid() const noexcept;
	WtChunkResourceCacheStatus insert_mesh(
		std::shared_ptr<const WtChunkMeshResult> mesh,
		WtGenerationToken generation,
		WtGenerationToken current_generation
	);
	WtChunkResourceCacheStatus insert_render(
		std::shared_ptr<const WtRenderPayload> render,
		WtGenerationToken current_generation
	);
	WtChunkResourceCacheStatus insert_collision(
		std::shared_ptr<const WtCollisionPayload> collision,
		WtGenerationToken current_generation
	);

	std::shared_ptr<const WtChunkMeshResult> find_mesh(
		const WtChunkKey &key,
		WtGenerationToken generation
	);
	std::shared_ptr<const WtRenderPayload> find_render(
		const WtChunkKey &key,
		WtGenerationToken generation
	);
	std::shared_ptr<const WtCollisionPayload> find_collision(
		const WtChunkKey &key,
		WtGenerationToken generation
	);
	WtChunkResourceCacheStatus find_or_rebuild_collision(
		const WtChunkKey &key,
		WtGenerationToken generation,
		const WtCollisionPolicy &policy,
		std::shared_ptr<const WtCollisionPayload> &collision,
		bool regular_only = false
	);

	std::size_t erase_key(const WtChunkKey &key);
	void clear() noexcept;

	std::size_t mesh_entry_count() const noexcept;
	std::size_t mesh_resident_bytes() const noexcept;
	std::size_t render_entry_count() const noexcept;
	std::size_t render_resident_bytes() const noexcept;
	std::size_t collision_entry_count() const noexcept;
	std::size_t collision_resident_bytes() const noexcept;
	WtChunkResourceCacheMetrics get_metrics() const noexcept;

private:
	struct MeshEntry {
		WtChunkKey key;
		WtGenerationToken generation;
		std::shared_ptr<const WtChunkMeshResult> payload;
		std::size_t resident_bytes = 0;
		std::uint64_t last_access = 0;
	};

	struct RenderEntry {
		WtChunkKey key;
		WtGenerationToken generation;
		std::shared_ptr<const WtRenderPayload> payload;
		std::size_t resident_bytes = 0;
		std::uint64_t last_access = 0;
	};

	struct CollisionEntry {
		WtChunkKey key;
		WtGenerationToken generation;
		std::shared_ptr<const WtCollisionPayload> payload;
		std::size_t resident_bytes = 0;
		std::uint64_t last_access = 0;
	};

	std::uint64_t next_access() noexcept;
	std::vector<MeshEntry>::iterator find_mesh_entry(
		const WtChunkKey &key,
		WtGenerationToken generation
	);
	std::vector<RenderEntry>::iterator find_render_entry(
		const WtChunkKey &key,
		WtGenerationToken generation
	);
	std::vector<CollisionEntry>::iterator find_collision_entry(
		const WtChunkKey &key,
		WtGenerationToken generation
	);
	void evict_mesh_to_limits();
	void evict_render_to_limits();
	void evict_collision_to_limits();

	WtChunkResourceCacheLimits limits_;
	bool valid_ = false;
	std::uint64_t access_counter_ = 0;
	std::size_t mesh_resident_bytes_ = 0;
	std::size_t render_resident_bytes_ = 0;
	std::size_t collision_resident_bytes_ = 0;
	std::vector<MeshEntry> meshes_;
	std::vector<RenderEntry> renders_;
	std::vector<CollisionEntry> collisions_;
	WtChunkResourceCacheMetrics metrics_;
};

} // namespace world_transvoxel
