# World Transvoxel 1.0 Public API

This document defines the supported Godot-facing API for the official
MIT-backed release. Methods whose names begin with `_m3_` or `_m5_` are
qualification hooks, not public API, and may change or disappear.

## `WorldTransvoxelTerrain`

`WorldTransvoxelTerrain` is a `Node3D`. Set a valid
`WorldTransvoxelConfig` before starting a world.

Identity and capability:

- `get_addon_version() -> String`
- `get_milestone() -> String`
- `is_mit_backend_available() -> bool`
- `get_backend_id() -> String`
- `get_backend_license() -> String`
- `get_backend_upstream_revision() -> String`

Configuration and lifecycle:

- `configuration: WorldTransvoxelConfig`
- `render_material_override: Resource`
- `is_configuration_valid() -> bool`
- `get_configuration_error() -> String`
- `set_render_material_override(material)` / `get_render_material_override()`
- `start_world(world_manifest_path, object_root) -> bool`
- `start_procedural_world(chunk_count_x, chunk_count_z, seed, source_revision, object_root) -> bool`
- `start_procedural_world_preset_with_vertical_origin(chunk_count_x, chunk_count_y, chunk_origin_y, chunk_count_z, seed, source_revision, preset_id, object_root) -> bool`
- `start_flat_world(chunk_count_x, chunk_count_z, source_revision, object_root) -> bool`
- `stop_world() -> bool`
- `get_world_state() -> int`
- `get_world_state_name() -> String`
- `is_world_running() -> bool`
- `get_world_error() -> String`
- `get_world_source_revision() -> int`
- `get_world_revision() -> int`
- `get_world_page_count() -> int`

Lifecycle state values are `0 stopped`, `1 starting`, `2 running`,
`3 stopping`, and `4 failed`. Startup and shutdown are asynchronous; observe
`world_state_changed`.

`start_procedural_world()` starts a compact deterministic horizontal chunk grid
without reading a `.wtworld` manifest. It generates requested page bytes on
demand through the same native page format, cache, meshing, editing, and
streaming pipeline used by manifest-backed worlds. The supported procedural
descriptor emits a bounded LOD0..LOD3 hierarchy, up to 262,144 indexed hierarchy
pages, with persistent edits stored in the object root journal.

`start_procedural_world_preset_with_vertical_origin()` starts the same native
procedural path with explicit vertical chunk coverage and a named procedural
preset. Supported preset IDs are:

- `mountain_reference` / `deterministic_reference`: the default deterministic
  mountain stress terrain;
- `rolling_hills_cave`: a bounded rolling-hills inspection terrain with a
  world-distance cave field, a road-aligned descending surface portal, compact
  smooth density subtraction, and connected underground chambers. It is
  intended for terrain-shape and mixed-LOD cave inspection profiles;
- `rolling_hills_cave_roads`: the same cave terrain plus a deterministic,
  volumetric road-network field. The field grades the solid density volume
  through blended shoulders and assigns material ID `10` to the top three
  world units inside each six-unit road corridor. It is a reference source for
  testing road intersections, chunk boundaries, collision, and mixed LODs;
  it is not a chain of runtime paint stamps;
- `four_biomes_lakes_caves_roads` / `g23_four_biomes_lakes_mountains_roads`:
  a 2048-by-2048 reference world with four categorical, non-mixing surface
  regions (grass, sand, gravel, and snow), three material-ID `9` lake volumes,
  three compact surface-connected caves, detailed rolling terrain and snow
  mountains, and one connected 18-segment material-ID `10` road graph. Lake,
  cave, terrain, and road fields are evaluated from world coordinates by the
  native source, so chunk and LOD boundaries do not redefine them.

`start_flat_world()` starts the same native procedural/storage/streaming path
with a flat surface at y=8. It is intended for baseline playtests and games
whose default terrain should be flat while retaining full voxel volume,
editing, collision, and LOD behavior.

`render_material_override` is an optional Godot material resource applied by the
native render sink to every existing and newly created render chunk. Higher-level
addons should prefer this over frame-by-frame recursive material scans so newly
streamed chunks do not flash with the engine default material.

Render meshes expose separate generated and authored material weights for that
override:

- `UV2.x` is the primary authoritative material ID for the rendered vertex.
- `UV2.y` is `1` when that material was explicitly authored by an edit and `0`
  when it still comes from the base source.
- `CUSTOM0` and `CUSTOM1` store generated-source weights for material IDs
  `1,2,3,4` and `5,7,8,10` respectively.
- `CUSTOM2` and `CUSTOM3` store explicitly authored edit weights for the same
  two material groups.

The authored flag is persisted with samples and follows the same solid
isosurface endpoint as the material ID. It lets a higher-level material keep
edited material categorical while applying an LOD-stable presentation derived
from a known procedural source only to unedited source material. The
custom weights are deterministic render derivatives of material IDs.
Neither render channel is a second terrain authority, and neither may replace
stored material samples, edit journals, or authoritative sample queries.
Legacy pages without provenance are conservatively exposed as `UV2.y = 1`;
rebake the base and replay edits to recover source/edit provenance in schema
1.2.

Streaming and readiness:

- `update_viewer(viewer_id, revision, position, radius_chunks, maximum_lod=0) -> bool`
- `remove_viewer(viewer_id, revision) -> bool`
- `query_chunk_state(chunk_coordinate, lod) -> WorldTransvoxelChunkState`
- `get_rendered_chunk_count() -> int`
- `get_collision_chunk_count() -> int`

Viewer IDs and revisions are positive. Revisions must increase for each
viewer. `position` is in world sample coordinates.

Editing:

- `begin_edit_transaction(author_id=0) -> WorldTransvoxelEditTransaction`
- `commit_edit_transaction(transaction) -> bool`

Queries and side-by-side snapshots:

- `request_authoritative_sample(grid_point, lod=0) -> int`
- `request_authoritative_samples(grid_points, lod=0) -> int`
- `request_world_compaction(output_directory, new_source_revision) -> int`
- `request_world_migration(output_directory) -> int`

Successful asynchronous requests return a positive request ID. Zero indicates
immediate rejection; inspect `get_world_error()`.

Application budgets and metrics:

- `set_render_apply_budget(budget)` / `get_render_apply_budget()`
- `set_collision_apply_budget(budget)` / `get_collision_apply_budget()`
- `get_render_resource_count() -> int`
- `get_collision_resource_count() -> int`
- `get_queued_render_count() -> int`
- `get_queued_collision_count() -> int`
- `get_render_latency_frames_maximum() -> int`
- `get_collision_latency_frames_maximum() -> int`
- `get_runtime_metrics() -> Dictionary`

The metrics dictionary includes `pending_chunk_retirements`, the number of old
chunk records/resources retained until the current replacement set is fully
ready, and `pending_chunk_replacements`, the number of same-key edit
replacements waiting for their render/collision resources before publication.
`visual_ready_chunk_records` and `fully_ready_chunk_records` provide explicit
settlement counts against `active_chunk_records`. Pending retirements and
pending replacements must both return to zero, and fully-ready must equal active,
after streaming settles.

Signals:

- `world_state_changed(state, state_name)`
- `world_failed(error)`
- `edit_committed(world_revision)`
- `edit_failed(error)`
- `authoritative_sample_ready(request_id, sample)`
- `authoritative_sample_failed(request_id, error)`
- `authoritative_samples_ready(request_id, samples)`
- `authoritative_samples_failed(request_id, error)`
- `world_snapshot_ready(request_id, manifest_path, source_revision, world_revision, page_count)`
- `world_snapshot_failed(request_id, error)`

## `WorldTransvoxelConfig`

This `Resource` exposes the construction-time capacities documented in
`OPERATING_LIMITS.md`. It provides `get_schema_version()`, `is_valid()`, and
`get_validation_error()`. Configuration is copied when startup begins; stop
the world before replacing it.

`lod_refinement_radius_chunks` is optional and defaults to `0`, which preserves
the legacy behavior where each viewer's `radius_chunks` controls both coarse
coverage and near-detail refinement. Set it above zero to cap refinement while
keeping a larger coarse LOD coverage radius. Large procedural worlds should use
this instead of forcing every visible root chunk to refine around the player.

`render_transition_frames` is an opt-in integer. The default is `0`, which
directly swaps replacement render meshes and avoids edit-time fade/blink. Values
above zero keep retiring render instances alive and fade replacement render
chunks over that many frames.

`shader_fade_parameter_enabled` is an opt-in boolean. The default is `false`.
Set it to `true` only when `render_transition_frames` is positive and a custom
shader declares and consumes `wt_fade_opacity`.

## `WorldTransvoxelEditTransaction`

Supported commands:

- `add_density_sphere(center, radius, value)`
- `set_density_sphere(center, radius, value)`
- `paint_material_sphere(center, radius, material)`
- `add_density_box(minimum, maximum, value)`
- `set_density_box(minimum, maximum, value)`
- `paint_material_box(minimum, maximum, material)`

All return `bool`. Inspect `get_error()` after rejection. Transactions also
expose base/committed revision, command count, and submitted state. A
transaction is single-use and stale base revisions are rejected.

## Immutable snapshots

`WorldTransvoxelChunkState` exposes presence, chunk coordinate, LOD,
generation, visual readiness, collision requirement/readiness, and full
readiness.

`WorldTransvoxelSample` exposes grid point, LOD, density, material, source
revision, world revision, and agreeing-page count.

Batch authoritative sample requests return an `Array` of
`WorldTransvoxelSample` objects in the same order as the submitted grid points.

These objects are point-in-time snapshots; they do not update in place.
