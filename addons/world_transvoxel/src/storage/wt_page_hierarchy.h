#pragma once

#include "core/wt_chunk_key.h"
#include "storage/wt_procedural_world_descriptor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

enum class WtPageHierarchyKind : std::uint8_t {
	Invalid,
	ExplicitCatalog,
	ImplicitProcedural,
};

struct WtPageHierarchyMetrics {
	WtPageHierarchyKind kind = WtPageHierarchyKind::Invalid;
	std::uint64_t declared_page_count = 0;
	std::uint64_t explicit_index_entries = 0;
	std::uint64_t estimated_index_bytes = 0;
	std::uint64_t membership_queries = 0;
	std::uint64_t child_queries = 0;
	std::uint64_t ancestor_queries = 0;
	std::uint64_t neighbor_queries = 0;
	std::uint64_t range_queries = 0;
	std::uint64_t viewer_root_queries = 0;
	std::uint64_t lod_enumerations = 0;
};

class WtPageHierarchy {
public:
	static WtPageHierarchy explicit_catalog(std::vector<WtChunkKey> keys);
	static WtPageHierarchy implicit_procedural(
		const WtProceduralWorldDescriptor &descriptor
	);

	bool valid() const noexcept;
	WtPageHierarchyKind kind() const noexcept;
	std::size_t page_count() const noexcept;
	std::size_t lod_page_count(std::uint8_t lod) const noexcept;
	bool contains(const WtChunkKey &key) const noexcept;
	bool complete_children(
		const WtChunkKey &parent,
		std::array<WtChunkKey, 8> &children
	) const noexcept;
	bool ancestor(
		const WtChunkKey &key,
		std::uint8_t ancestor_lod,
		WtChunkKey &output
	) const noexcept;
	bool append_face_neighbors(
		const WtChunkKey &key,
		std::vector<WtChunkKey> &output,
		std::size_t capacity
	) const;
	bool append_lod_keys(
		std::uint8_t lod,
		std::vector<WtChunkKey> &output,
		std::size_t capacity
	) const;
	bool query_range(
		const WtChunkKey &minimum,
		const WtChunkKey &maximum,
		std::vector<WtChunkKey> &output,
		std::size_t capacity
	) const;
	bool query_viewer_roots(
		const WtChunkKey &center,
		std::uint32_t radius,
		std::vector<WtChunkKey> &output,
		std::size_t capacity
	) const;
	WtPageHierarchyMetrics metrics() const noexcept;

private:
	WtPageHierarchyKind kind_ = WtPageHierarchyKind::Invalid;
	std::vector<WtChunkKey> explicit_keys_;
	WtProceduralWorldDescriptor procedural_descriptor_;
	std::array<std::size_t, kWtMaximumLod + 1> lod_counts_{};
	mutable WtPageHierarchyMetrics metrics_;
	bool valid_ = false;
};

const char *wt_page_hierarchy_kind_name(WtPageHierarchyKind kind) noexcept;

} // namespace world_transvoxel
