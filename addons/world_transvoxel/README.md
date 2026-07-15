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
60-second soak also pass. PQ0 through PQ2 complete production configuration,
lifecycle, real baked-world streaming, public editing, authoritative queries,
compaction, migration, and reopen equivalence. PQ3 is complete: a copied addon
passes a 15-second full-world workflow on Godot 4.6.3 and 4.7 with debug and
release binaries. PQ4 is complete. Version 1.0.0 is withdrawn because its
Godot render/collision winding and convex mixed-LOD corner deformation were
incorrect. Version 1.0.1 is superseded because moving-viewer plan changes
could expose transient replacement holes. Version 1.0.2 stages old chunks
until replacement render/collision readiness. Version 1.0.3 retains that
runtime behavior and replaces whole-source/all-page bake allocation with
bounded file-backed sampling and one-page output. Version 1.0.4 retains the
bounded baker and caps ready chunk retirement removal per frame. Version 1.0.5
adds a fixed native render fade-out window for retiring chunks after replacement
application. Version 1.0.6 also fades newly introduced render chunks through a
fixed native window. Version 1.0.7 extends both native render fade windows to
24 frames. Version 1.0.8 adds native same-key render mesh replacement crossfade
so an already visible chunk key does not swap mesh data at full opacity. Version
1.0.9 also publishes the per-instance shader parameter `wt_fade_opacity` so
custom terrain shaders can participate in the same native fade contract; it is
qualified for Windows x86-64 with both engines. Version 1.0.10-dev is a local
post-PQ4 S1 development build that adds a native batched authoritative sample
query for exact terrain-edit restoration. Version 1.0.11-dev is a local S2
development build that makes `wt_fade_opacity` instance-parameter writes
opt-in/default-off because Godot retains per-instance shader-parameter slots
after use. Version 1.0.12-dev also makes native render transition fading
opt-in/default-off; the default replacement path is a direct swap so terrain
edits do not blink. Version 1.0.13-dev stabilizes mesh-finalizer edge ownership
with quantized position keys at 1/1024 world-unit precision. Version
1.0.14-dev endpoint-regularizes isosurface interpolation to `[1/32, 31/32]`
before canonical chunk positioning and normal interpolation, replacing the old
M2 mesh hash with `02f60fe4c93375f9` and resolving the reproduced closed
edited-surface sliver warning without deleting surface triangles. These
development builds are not the deterministic PQ4 release artifact. The release
ships API/limit documentation, addon-local bake/storage wrappers, runtime
DLLs, and native tools. Compute acceleration is optional later work.

Procedural startup supports explicit vertical coverage through
`start_procedural_world_with_vertical_origin()` and
`start_flat_world_with_vertical_origin()`. The default legacy overloads still
use 8 vertical chunks at origin 0. Larger bounded volumes must pass explicit
`chunk_count_y` and `chunk_origin_y`; the current deep 2K proof profile uses
`128 x 16 x 128` LOD0 chunks and vertical origin `-8`. The native page/catalog
ceiling is `524288` pages so this 2K x 256 reference volume can start as real
volumetric terrain.

Build from the repository root:

```console
python scripts/build.py
```

Validate the production release and all prior milestone contracts:

```console
python scripts/test_production_qualification.py
python scripts/test_pq3.py
python scripts/test_pq4.py
python tools/benchmark_m5_application.py --engine-version 4.6.3
```
