# World Transvoxel Addon

This directory is the self-contained production addon boundary.

M0 through M2 provide:

- an isolated official MIT Transvoxel table source;
- a project-owned typed, fixed-capacity native backend interface;
- official regular and transition-cell meshing;
- all six right-handed transition orientations;
- reusable thread-local scratch storage;
- exhaustive debug/release native contract tests and deterministic hashes;
- signed chunk keys, 2:1 LOD validation, and deterministic transition ownership;
- bounded event-driven jobs, cancellation, and stale-result rejection;
- regular chunk meshing and all face, edge, and corner seam galleries;
- a minimal `WorldTransvoxelTerrain` GDExtension class;
- host-aware Python build orchestration using Zig and SCons;
- validated Windows x86-64 debug and release builds.

M3 adds deterministic render payloads, Godot `ArrayMesh` resources, sanitized
concave collision, distance hysteresis, bounded main-thread application,
generation-checked readiness, and application-latency telemetry.

M4 adds versioned world/chunk/edit storage, deterministic dense-volume baking,
content-addressed manifests, atomic edit transactions, padded spatial
invalidation, append-only replay, authoritative sample application, audited
compaction, migration/inspection tools, and a thin editor bake entry.

M5 production streaming is complete. Its asynchronous immutable-page storage
and all five bounded native cache tiers are implemented. The bounded
multi-viewer desired-set union, edit-driven generation replacement,
representative native budgets, and page-backed transition job scheduling,
pinning, cancellation, and invalidation are also complete. Real Godot
render/physics application budgets and the collision/readiness baseline are
locked on both supported engines. Versioned binary telemetry and the checked
60-second soak also pass. Production qualification is active; schema-1
`WorldTransvoxelConfig` is the first completed foundation, while real world
manifest startup/shutdown completes PQ0. PQ1 is complete: the root example
streams real baked LOD0 and balanced multi-LOD transition pages through native
scheduling, official MIT meshing, and bounded Godot sinks. PQ2 public editing,
query, and persistence integration is next. Compute acceleration is optional
later work.

Build from the repository root:

```console
python scripts/build.py
```

Validate the active production qualification foundation and all prior
milestone contracts:

```console
python scripts/test_production_qualification.py
python tools/benchmark_m5_application.py --engine-version 4.6.3
```
