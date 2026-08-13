# Production Read-Only Streaming Contract

Status: complete; closes with the production example contract

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

Refinement selection has a one-child-chunk exit hysteresis shell. A subtree
that is already refined remains refined until every viewer has moved beyond
the configured refinement radius plus that shell; entering an unrefined
subtree still uses the configured radius exactly. Hysteresis is derived from
the current desired leaves and changes only temporal retention, never the
balanced 2:1 topology or transition ownership.

`radius_chunks` defines the root search cube and near-view refinement distance.
`(2 * radius + 1)^3` must fit `demand_capacity_per_viewer`. Final global leaves
must fit `active_chunk_capacity`. `remove_viewer(viewer_id, revision)` rebuilds
the same global plan without that viewer.

Distance priority and collision hysteresis are evaluated against every active
viewer. GDScript owns no page catalog, chunk map, balancing loop, or meshing
work.

The production non-flat integration profile uses a three-child-chunk
refinement radius. With 16-cell LOD0 chunks, a newly entered LOD0-to-coarse
boundary therefore cannot be selected closer than 48 base-grid units; an
already refined subtree has one additional 16-unit exit shell. This is a
quality policy, not a substitute for surface shifting.

## Edited terrain LOD retention boundary

The detailed edited-terrain rule is
`PRODUCTION_EDITED_TERRAIN_LOD_CORRECTNESS_CONTRACT.md`. Edited terrain LOD
retention is part of the streaming contract, but it is not an unlimited promise
that every edit remains at near-detail LOD from every future camera distance.
The runtime remembers recent edit-retention zones and injects them into planning
as temporary retention viewers. When a full retention plan is too expensive,
planning must degrade retention in priority order instead of dropping all
retention:

1. keep newest/visible edit zones first;
2. reduce edit-retention refinement radius when needed;
3. fall back to no edit-retention only if no degraded plan fits.

This boundary is serious for games with mining, tunneling, or player-made
terrain structures. A distant edited hole may still simplify if the project
chooses budgets that cannot keep that edit refined. That is acceptable only when
the game has explicitly chosen that tradeoff and has validated the resulting
visual behavior. It is not acceptable for fallback pressure to silently erase all
edit-retention viewers while reporting a settled runtime.

Downstream projects must carry this contract forward. Any profile that claims
seamless edited terrain must include close/mid/far/back movement validation after
player-like edits and must record whether edit-retention remained active,
whether persistence samples changed, and whether rendered geometry stayed free of
open/nonmanifold gaps.

## Thread and queue ownership

The lifecycle control thread owns the planner, desired set, scheduler,
page/resource caches, page meshing runtime, official MIT mesher, and worker-side
application state. Async storage completion signals wake that thread without
frame polling. Viewer events are bounded and newer pending events for one
viewer coalesce.

Immutable render/collision payloads and chunk-control events cross to the Godot
frame thread through bounded priority and normal publication rings. The facade
defers a render or collision payload when its corresponding frame budget is
exhausted. Lightweight expectation, requirement-change, and removal controls
may overtake that budget-blocked payload so a zero resource budget cannot
head-of-line block lifecycle state. Remaining payload order is preserved, and
generation checks make any overtaken obsolete payload stale before a sink is
called.

Application records are shared synchronization state between the lifecycle and
frame threads. Record mutation and snapshot reads are mutex-serialized; no
production path retains a pointer into the mutable record vector across that
boundary. Readiness repair publication is idempotent per chunk generation, so
holding application budgets at zero cannot generate an unbounded duplicate
render or collision stream.

Godot objects and physics resources are created, replaced, and removed only on
the frame thread.

## Atomic cross-LOD publication

A replacement that overlaps a pending ancestor or descendant retirement is not
independently publishable. The frame facade builds the exact transitive overlap
component from indexed dyadic ancestor/descendant lookups. Unrelated pending
replacements and retirements do not join that component, even under a large
streaming backlog.

The component publishes only when every replacement generation has its visual
payload and, when required, its collision payload staged. Retirements and both
replacement sinks are then applied as one frame-thread operation. An open
viewer-plan publication prevents all staged publication, so a plan cannot be
observed halfway through its control-event sequence.

If an exact component is incomplete, the facade requests interactive priority
for each missing `(chunk key, generation)` once. The lifecycle runtime raises
only matching scheduler, page-meshing, and queued storage work. A stale
generation request is counted and ignored. Priority changes ordering only; they
do not relax complete coverage, generation checks, 2:1 topology, transition
ownership, or paired render/collision publication.

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
generation replacement, page-backed official MIT transition meshing, edit-LOD
fallback retention under planner pressure, and shutdown. Debug and release hash:

```text
7c9071d1c31da0397e48db0338c299b8bf923dbb0e80448ae11cf049c3c0d2fb
```

`production_streaming_test.gd` and `production_lod_streaming_test.gd` prove
the public API creates, replaces, and removes real `ArrayMesh` and
`ConcavePolygonShape3D` resources on Godot 4.6.3 and 4.7 with debug and release
addon builds. The LOD fixture also holds both resource budgets at zero while a
mixed replacement plan is published, proving control-event bypass, retained-old
terrain, and bounded per-generation readiness repair. The production entry
point then runs the complete M5-through-M0 regression chain.

The production_lod_edit_atomicity_test.gd fixture adds the movement-plus-edit
case: an independently publishable edit that overlaps a pending cross-LOD
retirement is promoted to regional publication. The Godot 4.7 debug and release
route requires matched visible render/collision ownership throughout the swap.
`test_wt_m3_application.cpp` additionally proves exact component discovery with
2,048 unrelated replacements and 2,048 unrelated retirements. The production
streaming fixture proves matching-generation priority application and stale
generation rejection in an isolated runtime.

## PQ1 exit

The thin root `world_transvoxel/` example consumes this coordinator in a real
scene and passes the debug/release Godot matrix. PQ1 is complete. Editing,
query, and persistence are the finite PQ2 scope.
