# M3 Status: Godot Render and Collision Integration

Status: complete

Completed: 2026-06-20

Validated platform: Windows x86-64

Validated engines: Godot 4.6.3 and Godot 4.7

## Result

M2 chunk output now crosses a typed, bounded native-to-Godot boundary.
Worker-safe queues carry immutable render and collision payloads; the main
thread rejects stale generations before creating or replacing any Godot
resource.

The render sink creates one `ArrayMesh`/`MeshInstance3D` per active chunk.
Positions remain chunk-local, the instance carries the chunk world origin,
normals are preserved, and categorical material IDs are available through
`UV2.x`. Replacement reuses the chunk node instead of accumulating resources.

The physics path builds a separate triangle-face payload, removes degenerate
and scale-independently thin triangles, and creates a
`ConcavePolygonShape3D` under a `StaticBody3D`. Collision demand uses tested
activation/deactivation hysteresis and independent readiness.

The normative definition is
[`docs/contracts/M3_RENDER_COLLISION_CONTRACT.md`](../contracts/M3_RENDER_COLLISION_CONTRACT.md).

## Evidence

Command:

```console
python scripts/test_m3.py
```

The native debug and optimized tests cover:

- deterministic regular/transition render-buffer combination;
- inactive transition and malformed geometry rejection;
- collision degeneracy and thin-triangle sanitation with separate metrics;
- collision-distance hysteresis boundaries;
- bounded render/collision queues and application records;
- zero and independent render/collision frame budgets;
- readiness transitions and sink failures;
- stale rejection before sink mutation;
- total and maximum application latency in frame ticks;
- 1,000 supersession cycles with bounded state.

The headless Godot fixture uses an actual M2-generated sphere and runs against
debug and release addon binaries under both supported Godot versions. It
covers stale generation replacement, one-item budgets, `ArrayMesh` and concave
collision creation, 16 near/far movement cycles, bounded resource counts,
in-flight eviction, and shutdown with queued work. No GPU readback or
frame-thread wait is used.

Methods prefixed `_m3_test_` are internal acceptance-fixture hooks and are not
part of the supported terrain API.

Repository validation passes with no source-size or license-boundary warnings.

## Scope boundary

M3 proves resource preparation, main-thread application, collision sanitation,
movement-driven collision readiness, replacement, and teardown. It does not
yet provide persistent worlds, deterministic baking, edit transactions,
production cache eviction, representative soak budgets, or compute
acceleration.

M4 storage, baking, and editing subsequently completed. The next and only
active milestone is M5: streaming production baseline.
