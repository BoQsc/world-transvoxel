# Production Read-Only Streaming Contract

Status: LOD0 production foundation complete; PQ1 remains active

## Scope

This contract is the first production use of the completed M5 components. A
running `WorldTransvoxelTerrain` accepts native viewer events and streams real,
non-empty baked LOD0 pages through:

```text
viewer event -> desired set -> scheduler -> async page I/O -> page cache
             -> official MIT mesher -> resource cache -> publication queue
             -> bounded Godot render/collision application
```

No M3/M5 fixture generates the runtime mesh. The test fixture is written by
the real baker and then consumed through the same manifest/object paths as an
application world.

## Viewer API

`update_viewer(viewer_id, revision, position, radius_chunks)` records a
monotonic viewer snapshot. The native runtime plans the LOD0 cube around the
viewer, filters absent keys through the manifest index, prioritizes nearer
pages, and applies collision hysteresis. `(2 * radius + 1)^3` must fit the
configured per-viewer demand capacity.

`remove_viewer(viewer_id, revision)` removes its demand. Multiple viewers are
merged by the bounded native desired-set service; GDScript never owns chunk
maps, scans page catalogs, or executes meshing loops.

## Thread and queue ownership

The lifecycle control thread owns the scheduler, page/resource caches, page
meshing runtime, official MIT mesher, and a worker-side application state.
Async storage completion signals wake that thread without frame polling.
Viewer events are bounded and newer pending events for one viewer coalesce.

Immutable render/collision payloads and chunk-control events cross to the
Godot frame thread through a bounded publication ring. The facade defers a
payload when its corresponding frame budget is exhausted, preserving event
order without overflowing the application queues. Godot objects and physics
resources are created, replaced, and removed only on the frame thread.

Stopping wakes all waits, joins native ownership, discards deferred work, and
clears every Godot render/collision resource.

## Proof

`test_wt_production_streaming.cpp` bakes four non-empty plane pages and proves
manifest lookup, async I/O, official MIT meshing, cache publication, movement,
underground/vertical viewer positions, two-viewer union, eviction, and clean
shutdown. Debug and release hashes must match:

```text
39db05c67fc2f4b8d8beaab2e7da927ae968efb3d75118bcd80c5523116d9b3b
```

`production_streaming_test.gd` proves the same public path creates and removes
real `ArrayMesh` and `ConcavePolygonShape3D` resources on Godot 4.6.3 and 4.7
with both addon builds. The production entry point then runs the complete
M5-through-M0 regression chain.

## Exact remaining PQ1 boundary

This unit intentionally plans LOD0 only. PQ1 is not complete until two finite
units pass:

1. a native balanced multi-LOD planner drives the existing 2:1 map and
   page-backed transition meshing through this coordinator;
2. the thin root `world_transvoxel/` example proves that planner in a real
   scene and closes the PQ1 exit audit.

Editing, query, and persistence remain PQ2.
