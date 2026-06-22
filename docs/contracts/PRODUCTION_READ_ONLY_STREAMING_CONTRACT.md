# Production Read-Only Streaming Contract

Status: regular and balanced multi-LOD transition streaming complete; PQ1
root example remains

## Scope

A running `WorldTransvoxelTerrain` accepts native viewer events and streams
real baked pages through:

```text
viewer event -> global balanced LOD plan -> desired set -> scheduler
             -> async page I/O -> page cache -> official MIT mesher
             -> resource cache -> publication ring
             -> bounded Godot render/collision application
```

No M3/M5 fixture generates the runtime mesh. Both production fixtures are
written by the real baker and consumed through the same manifest and
content-addressed object paths as an application world.

## Viewer and LOD API

`update_viewer(viewer_id, revision, position, radius_chunks, maximum_lod = 0)`
records a monotonic snapshot. `maximum_lod` selects the coarsest requested
root level; zero preserves the original LOD0 behavior. The planner uses the
highest requested level across active viewers to build one global map, filters
roots through the manifest catalog, and recursively replaces a coarse page by
all eight children near a viewer when that complete child set exists.

The native LOD map rejects overlapping leaves and enforces a maximum one-level
difference across face neighbors. When balancing needs unavailable children
or would exceed `active_chunk_capacity`, the viewer event is rejected without
committing a new plan. Coarse leaves own transition faces toward finer leaves.
A transition-mask change on a retained key increments its generation and
forces remeshing; it is not treated as a priority-only update.

`radius_chunks` defines the root search cube and near-view refinement distance.
`(2 * radius + 1)^3` must fit `demand_capacity_per_viewer`. Final global leaves
must fit `active_chunk_capacity`. `remove_viewer(viewer_id, revision)` rebuilds
the same global plan without that viewer.

Distance priority and collision hysteresis are evaluated against every active
viewer. GDScript owns no page catalog, chunk map, balancing loop, or meshing
work.

## Thread and queue ownership

The lifecycle control thread owns the planner, desired set, scheduler,
page/resource caches, page meshing runtime, official MIT mesher, and worker-side
application state. Async storage completion signals wake that thread without
frame polling. Viewer events are bounded and newer pending events for one
viewer coalesce.

Immutable render/collision payloads and chunk-control events cross to the Godot
frame thread through a bounded publication ring. The facade defers a payload
when its corresponding frame budget is exhausted, preserving event order.
Godot objects and physics resources are created, replaced, and removed only on
the frame thread.

Stopping wakes all waits, joins native ownership, discards deferred work, and
clears every Godot render/collision resource.

## Proof

`test_wt_production_streaming.cpp` bakes four non-empty LOD0 plane pages and
proves manifest lookup, async I/O, official MIT regular meshing, movement,
underground/vertical positions, two-viewer union, eviction, and shutdown.
Debug and release hash:

```text
39db05c67fc2f4b8d8beaab2e7da927ae968efb3d75118bcd80c5523116d9b3b
```

`test_wt_production_lod_streaming.cpp` bakes 28 hierarchical LOD1/LOD0 plane
pages and proves bounded planning, capacity/duplicate rejection, 2:1 leaves,
coarse transition ownership, two-viewer balancing, movement, transition-mask
generation replacement, page-backed official MIT transition meshing, and
shutdown. Debug and release hash:

```text
cf0d7ca3f67013d99d0f909368d179aa6878a0bbd7b57ee409ac5c8994681102
```

`production_streaming_test.gd` and `production_lod_streaming_test.gd` prove
the public API creates, replaces, and removes real `ArrayMesh` and
`ConcavePolygonShape3D` resources on Godot 4.6.3 and 4.7 with debug and release
addon builds. The production entry point then runs the complete M5-through-M0
regression chain.

## Exact remaining PQ1 boundary

The only remaining PQ1 unit is the thin root `world_transvoxel/` example. It
must consume the addon as an application would, stream this planner in a real
scene, and close the PQ1 audit. Editing, query, and persistence remain PQ2.
