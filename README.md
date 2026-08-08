# World Transvoxel

Production-oriented Godot terrain research and implementation using the
official upstream MIT Transvoxel tables behind an isolated native addon.

## Release status

World Transvoxel 1.0.0 is withdrawn and must not be used. It adapted neither
render nor one-sided collision winding to Godot and did not implement complete
primary/secondary transition deformation at convex mixed-LOD corners.

World Transvoxel 1.0.1 is superseded because moving-viewer plan changes could
applied. World Transvoxel 1.0.3 is superseded because dynamic mixed-LOD viewer
motion could retire a large replacement set in one frame, producing visible LOD
swap popping. World Transvoxel 1.0.5 is superseded because it faded retiring
chunks but introduced replacement chunks abruptly. World Transvoxel 1.0.6 is
superseded because its 12-frame native crossfade still exceeded the sandbox
multi-view temporal gross-pop gate. World Transvoxel 1.0.7 is superseded
because same-key render mesh replacements could still swap at full opacity.
World Transvoxel 1.0.8 is superseded because opaque custom terrain shaders could
ignore native `GeometryInstance3D` transparency and still expose silhouette
changes during LOD replacement. World Transvoxel 1.0.9 is
the current official MIT-backed addon
for Windows x86-64 with Godot 4.6.3 and 4.7. It retains the topology fixes,
bounded application-confirmed chunk retirement, continuous-motion
render/collision regression coverage, bounded dense baking, and caps ready
chunk retirement removal to a small per-frame budget. It also fades retiring
and newly introduced render chunks for fixed 24-frame native windows after
replacement application, so visual chunks are not removed or introduced as a
one-frame render swap, and same-key render mesh replacement keeps the previous
mesh as a temporary retiring instance while the replacement mesh fades in. The
fade state can also be published to custom shaders through the opt-in
per-instance `wt_fade_opacity` parameter. Development build `1.0.12-dev`
keeps shader fade parameters and native render transition fading disabled by
default; replacement meshes direct-swap unless `render_transition_frames` is
set above zero. Development build `1.0.16-dev` applies endpoint regularization
before the cell backend removes degenerate triangles, preventing
exact-isovalue tangent poles from losing triangles that chunk canonicalization
cannot reconstruct. The correction is covered by cell-level and assembled
finite-window topology tests. The
deterministic install directory is:

```text
artifacts/release/world-transvoxel-1.0.9-windows-x86_64/
```

Copy its `addons/world_transvoxel/` directory and retain the included license
notices. The independent 0BSD backend is not the production backend and is not
claimed to be an exact replacement; it may now enter separate qualification
against this baseline.

The working tree may contain post-PQ4 development builds. Version
`1.0.10-dev` adds native batched authoritative sample queries for sandbox S1
terrain-edit latency work. Version `1.0.11-dev` clears default
`wt_fade_opacity` instance-parameter allocation by making writes opt-in/default-off
for S2 large-scale visual evidence. Version `1.0.12-dev` also makes native
render transition fading opt-in/default-off so editing does not blink by
default. Version `1.0.13-dev` added quantized finalizer connectivity keys.
Version `1.0.15-dev` superseded the old mesh hash with endpoint-regularized
chunk meshing and closed the retained pairwise smoothed-crater seam, but applied
the policy too late to retain every exact-isovalue cell triangle. Version
`1.0.16-dev` moves the same interpolation policy into the shared cell backend
and requires the assembled smoothed-crater window to have no interior openings
or non-manifold edges. These development builds are not the deterministic
1.0.9 release artifact.

## Canonical direction

Read [IMPLEMENTATION_CHARTER.md](IMPLEMENTATION_CHARTER.md) before changing the
project. It is the single authoritative statement of the intended product,
license boundaries, architecture, performance model, implementation roadmap,
acceptance tests, and production finish line.

If another project document conflicts with the charter, the charter controls.

## Critical edited-terrain LOD boundary

World Transvoxel streams a bounded active chunk set. Recent edit-retention keeps
newly dug or placed terrain refined longer as a player moves away, and fallback
pressure now degrades retention instead of dropping it all at once. This is still
a budgeted guarantee, not a promise that every edited region remains high-detail
from every distance. Games with mining, tunneling, or player-made terrain must
carry the edited terrain contract in
[docs/contracts/PRODUCTION_EDITED_TERRAIN_LOD_CORRECTNESS_CONTRACT.md](docs/contracts/PRODUCTION_EDITED_TERRAIN_LOD_CORRECTNESS_CONTRACT.md)
and the volumetric terrain contract in
[docs/contracts/PRODUCTION_TERRAIN_VOLUME_CONTRACT.md](docs/contracts/PRODUCTION_TERRAIN_VOLUME_CONTRACT.md)
and the runtime limits in
[addons/world_transvoxel/OPERATING_LIMITS.md](addons/world_transvoxel/OPERATING_LIMITS.md),
then validate their chosen camera distances, active chunk capacity, viewer
radius, and edit-retention behavior before claiming seamless edited terrain.

## Critical future multiplayer/server boundary

World Transvoxel is still a local terrain addon today, but current terrain
decisions must remain compatible with future multiplayer and large dedicated
servers. The authoritative boundary is
[docs/contracts/PRODUCTION_MULTIPLAYER_SERVER_COMPATIBILITY_CONTRACT.md](docs/contracts/PRODUCTION_MULTIPLAYER_SERVER_COMPATIBILITY_CONTRACT.md).
Terrain authority belongs to density/material pages, world revisions, edit
journals, snapshots, and validated edit transactions. Client meshes, LOD fades,
materials, collision presentation, and screenshots are derived data and must not
become saved or network-authoritative terrain state.

## Critical bounded vertical-volume boundary

Procedural worlds now expose native vertical chunk coverage instead of assuming
a fixed Y span. Use `start_procedural_world_with_vertical_origin()` or
`start_flat_world_with_vertical_origin()` when a game needs underground
coverage below Y=0. The current integration-game deep proof profile uses
`128 x 16 x 128` LOD0 procedural chunks, vertical chunk origin `-8`, and a
bounded `2048 x 2048 x 256` cell reference volume.

This is still bounded terrain. The native procedural/catalog page ceiling is
`524288` pages so the 2K x 256 profile, at roughly `299520` pages across LOD0
through LOD3, can start without falling back to a heightmap, duplicate render
surface, or presentation cap.

## Current phase

R0 through M5 are complete. The addon now has the exact native cell backend,
closed-seam chunk meshing, bounded scheduling, Godot `ArrayMesh` rendering,
sanitized concave collision, generation-checked application, and readiness
telemetry. M4 adds the versioned bounded
container, standalone authoritative chunk pages, native deterministic page
baker, content-addressed world manifest/index, and SHA-256 corruption checks.
Atomic typed edit transactions are also implemented; spatial
invalidation now covers padded same-LOD and coarser-LOD dependencies. Journal
ordering, append recovery, and deterministic replay are implemented.
Ordered commands now mutate authoritative chunk samples with overlap tests and
finite-result rejection. Snapshot compaction now emits audited replacement
pages and a revisioned world manifest. Native-backed Python storage
inspection, validation, and migration tools are implemented. The practical
dense-volume command-line baker is also implemented with file-backed source
sampling and one-page-at-a-time output. Editor scaffolding and
the editor Tools menu use the same Python/native bake path. The controlled
codec decision retains deterministic zero-copy `none` and rejects byte RLE.
M5 production streaming provides native asynchronous manifest/page loading
with content-addressed
validation, queue backpressure, and generation-tagged completions. Bounded
encoded-page and decoded-sample caches now add generation rejection,
immutable identities, byte/item budgets, and deterministic LRU eviction.
Derived mesh, render, and collision payloads now use the same bounded
generation-safe ownership model. Multi-viewer demand events now produce a
deterministic priority union and typed scheduling deltas without idle scans.
Committed edits now replace only spatially affected loaded generations,
preserve separate source/world revisions, evict authoritative and derived
cache ownership, reject stale pipeline results, and reset visual/collision
readiness until remeshing and application complete.
Large procedural worlds retain their declared LOD hierarchy as an implicit
descriptor instead of a full sorted key vector. Local durable edits compact to
a checksummed sparse overlay, reopen through `start_procedural_snapshot()`, and
preserve generated-page fallback for untouched regions. The runtime exposes
separate declared-hierarchy, sparse-index, overlay, cache, active-record, and
resource metrics. The retained 299,520-page reference compacts, reopens,
migrates, and replays central and finite-boundary edits without materializing
all page keys.
Desired-set deltas now create, reprioritize, and release bounded runtime
ownership. A deterministic workload covers fast movement, teleportation,
underground and vertical traversal, two viewers, collision-demand changes, and
edits while moving without record or queue growth.
Reference-hardware budgets now cover native orchestration, real
content-addressed warm page I/O/decode, decoded-page LOD0 MIT meshing, and
higher-LOD MIT transition meshing. Chunk-page schema 1.1 adds sparse validated
finest-edge records for Lengyel surface shifting; each coarse transition face
still deterministically owns four adjacent LOD-minus-one pages, and decoded-page
seams pass all face and corner contracts. Real runtime jobs now
request and pin those support pages, retry scheduler backpressure, discard late
cancelled completions, invalidate dependent coarse generations, and release
pins after official MIT meshing. Optimized Godot 4.6.3 and 4.7 evidence now
locks real `ArrayMesh`/concave-collision application budgets, eight-frame
burst readiness, and the 96/128 collision hysteresis policy. A versioned
bounded `wttrace` format and checked 60-second motion/edit soak now close M5.
Ready chunk retirements are also flushed through a bounded per-frame removal
budget. The released 1.0.9 path fades retiring and newly introduced render
chunks through fixed native windows; the current development path keeps that
capability behind opt-in `render_transition_frames` so editing defaults to
direct replacement.
Clean-addon installation and the real baked-world
stream/render/collide/edit/query/save/reload/migrate/shutdown workflow close
PQ3. PQ4 adds two-build byte reproducibility, installed tool validation,
license/provenance audit, shipped API/limit documentation, and exact-release
Godot matrix evidence. Optional compute acceleration remains deferred.

The current production audit is
[`docs/production/QUALIFICATION_AUDIT.md`](docs/production/QUALIFICATION_AUDIT.md).
Its first completed foundation is the schema-1 native/Godot runtime
configuration. Asynchronous manifest startup, failure, restart, and storage
shutdown completes PQ0. PQ1 is complete: the root `world_transvoxel/` scene
streams real baked LOD0 and balanced multi-LOD transition pages through
official MIT meshing into bounded Godot resources. PQ2 public editing, query,
and persistence integration is complete. It includes immutable
active-chunk/readiness snapshots, lifecycle-owned `world.wtedit`, typed public
transactions, durable commits, affected-generation replacement, edited-page
replay, stale rejection, exact authoritative scalar/material queries,
side-by-side snapshot compaction, schema-1.0 migration, and reopen
equivalence. PQ3 is complete: an isolated copied-addon project now passes the
15-second full-world edit/query/compact/migrate/reopen soak on Godot 4.6.3 and
4.7 with debug and release binaries. PQ4 is complete for 1.0.9, and the
official MIT-backed addon is production-ready within the documented Windows
x86-64 support matrix.

## Tooling

All project-owned build, download, validation, and test automation uses Python
3.11 or newer. The entry points share host detection and path/process safety;
there are no operating-system shell scripts in the project-owned toolchain.

```console
python scripts/bootstrap_toolchain.py
python scripts/build.py
python scripts/prepare_example_world.py
python scripts/test_m5.py
python scripts/test_pq3.py
python scripts/test_pq4.py
python scripts/build_release.py
python scripts/benchmark_sparse_hierarchy.py
python tools/benchmark_m5_runtime.py
python tools/benchmark_m5_pipeline.py
python tools/benchmark_m5_application.py --engine-version 4.6.3
python tools/run_m5_soak.py
python tools/wt_storage.py inspect path/to/world.wtworld
python tools/wt_storage.py validate path/to/page.wtchunk
python tools/wt_storage.py migrate-world old.wtworld current.wtworld
python tools/wt_bake.py density.f32 keys.txt output \
  --origin -1 -1 -1 --dimensions 35 19 19 \
  --spacing 1 --source-revision 7
```

## Production strategy

1. Start with the official upstream MIT `Transvoxel.cpp`.
2. Keep it unchanged and isolated inside `addons/world_transvoxel/thirdparty/`.
3. Access it through a project-owned backend interface.
4. Establish a long-running production baseline for terrain, collision,
   editing, LOD, streaming, and performance.
5. Keep alternate backends, including the independent 0BSD implementation,
   behind the same interface.
6. Switch only after an alternate backend passes the same acceptance suite.

## Engineering direction

- Godot GDExtension C++ owns hot paths, data, scheduling, meshing, collision
  preparation, storage, and telemetry.
- Compute shaders are used where work remains GPU-resident or is naturally
  batched. GPU readback is not treated as free.
- GDScript is limited to editor integration, scene wiring, examples, and
  non-critical scaffolding.
- World and edit data use explicit versioned binary formats.
- Runtime work is event-driven and budgeted; idle frames do not scan every
  chunk.
- Public API is intentionally small. Internally, responsibilities are split
  into typed services with predictable ownership.
- Source files are prefixed and size-limited. Vendor and generated files are
  isolated exceptions.

## Intended repository layout

```text
addons/world_transvoxel/       self-contained native addon
world_transvoxel/              thin example/application integration
docs/                          architecture, formats, decisions, roadmap
references/                    pinned source manifest and local downloads
tests/                         unit, exhaustive, integration, soak, performance
tools/                         bake, inspect, validate, and conversion tools
```

Start with:

```text
IMPLEMENTATION_CHARTER.md
docs/research/MARCHING_CUBES_POSTMORTEM.md
docs/research/REFERENCE_ASSESSMENT.md
docs/research/VOXEL_TOOLS_FINDINGS.md
docs/architecture/ARCHITECTURE.md
docs/architecture/API_BOUNDARIES.md
docs/architecture/BINARY_FORMATS.md
docs/architecture/CODING_STANDARDS.md
docs/ROADMAP.md
```

Download the pinned local research set:

```console
python scripts/download_references.py
```

Validate the repository and any locally downloaded references:

```console
python tools/validate_repository.py
```
