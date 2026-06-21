#pragma once

#include "physics/wt_collision_builder.h"

#include <cstddef>

namespace world_transvoxel {

bool wt_is_valid_mesh_payload(const WtChunkMeshResult &mesh) noexcept;
bool wt_equal_mesh_payload(
	const WtChunkMeshResult &left,
	const WtChunkMeshResult &right
) noexcept;
std::size_t wt_mesh_payload_resident_bytes(
	const WtChunkMeshResult &mesh
) noexcept;

bool wt_is_valid_render_payload(const WtRenderPayload &render) noexcept;
bool wt_equal_render_payload(
	const WtRenderPayload &left,
	const WtRenderPayload &right
) noexcept;
std::size_t wt_render_payload_resident_bytes(
	const WtRenderPayload &render
) noexcept;

bool wt_is_valid_collision_payload(
	const WtCollisionPayload &collision
) noexcept;
bool wt_equal_collision_payload(
	const WtCollisionPayload &left,
	const WtCollisionPayload &right
) noexcept;
std::size_t wt_collision_payload_resident_bytes(
	const WtCollisionPayload &collision
) noexcept;

} // namespace world_transvoxel
