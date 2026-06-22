# World Transvoxel

Production-oriented Godot terrain research and implementation using the
official upstream MIT Transvoxel tables behind an isolated native addon.

## Release status

World Transvoxel 1.0.0 is the production-ready official MIT-backed addon for
Windows x86-64 with Godot 4.6.3 and 4.7. The deterministic install directory
is generated at:

```text
artifacts/release/world-transvoxel-1.0.0-windows-x86_64/
```

Copy its `addons/world_transvoxel/` directory and retain the included license
notices. The independent 0BSD backend is not the production backend and is not
claimed to be an exact replacement; it may now enter separate qualification
against this baseline.

## Canonical direction

Read [IMPLEMENTATION_CHARTER.md](IMPLEMENTATION_CHARTER.md) before changing the
project. It is the single authoritative statement of the intended product,
license boundaries, architecture, performance model, implementation roadmap,
acceptance tests, and production finish line.

If another project document conflicts with the charter, the charter controls.

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
dense-volume command-line baker is also implemented. Editor scaffolding and
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
Desired-set deltas now create, reprioritize, and release bounded runtime
ownership. A deterministic workload covers fast movement, teleportation,
underground and vertical traversal, two viewers, collision-demand changes, and
edits while moving without record or queue growth.
Reference-hardware budgets now cover native orchestration, real
content-addressed warm page I/O/decode, decoded-page LOD0 MIT meshing, and
higher-LOD MIT transition meshing. Schema-1 remains unchanged: each coarse
transition face deterministically owns four adjacent LOD-minus-one pages, and
decoded-page seams pass all face and corner contracts. Real runtime jobs now
request and pin those support pages, retry scheduler backpressure, discard late
cancelled completions, invalidate dependent coarse generations, and release
pins after official MIT meshing. Optimized Godot 4.6.3 and 4.7 evidence now
locks real `ArrayMesh`/concave-collision application budgets, eight-frame
burst readiness, and the 96/128 collision hysteresis policy. A versioned
bounded `wttrace` format and checked 60-second motion/edit soak now close M5.
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
4.7 with debug and release binaries. PQ4 is complete, and the official
MIT-backed addon is production-ready within the documented Windows x86-64
support matrix.

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
