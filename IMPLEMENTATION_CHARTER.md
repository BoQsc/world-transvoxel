# World Transvoxel Implementation Charter

Status: canonical project direction

Last reviewed: 2026-06-25

## Physics-published collision patch bases

Incremental LOD0 collision preparation resolves its retained block base from
the frontend physics-residency identity, with the application record as a
generation-checked fallback. Candidate insertion and cache eviction preserve
the generation currently published to physics while newer generations are in
flight. Viewer-plan bookkeeping may not discard that base or force a complete
collision rebuild. Generation ordering and cache identity must match before a
partial patch is admitted; otherwise the runtime uses the complete
authoritative rebuild fallback.

Current state: M5 streaming production baseline complete on Windows x86-64
with bounded storage/caches, multi-viewer/edit runtime ownership, page-backed
official MIT meshing, real Godot render/physics application budgets, versioned
binary telemetry, and checked fixed-duration soak evidence. PQ0-PQ3 complete
configuration, lifecycle, real-world streaming, editing, query, persistence,
clean installation, and the fixed-duration full-world Godot matrix. PQ4
qualifies the deterministic World Transvoxel 1.0.5 directory release,
installed native tools, license/provenance boundary, public API, operational
limits, bounded dynamic chunk-retirement flushing, native retiring-render
fade-out, and exact release install matrix. The official MIT-backed addon is
production-ready on Windows x86-64 with Godot 4.6.3 and 4.7. Version 1.0.0 is
withdrawn for topology errors, and 1.0.1 is superseded because moving-viewer
replacement could expose transient render/collision holes.

## 1. Authority of this document

This document is the single source of truth from the first implementation
commit through the production finish line.

If another README, architecture note, issue, comment, experiment, or old
implementation conflicts with this document, this document controls. The
supporting files under `docs/` preserve research details and evidence, but a
contributor must be able to understand the intended product, architecture,
license boundary, implementation order, and acceptance criteria from this file
alone.

Any change that alters the end goal, license model, subsystem ownership,
persistent formats, public API direction, performance model, milestone order,
or definition of done must update this document in the same commit. Quiet
architectural drift is not accepted.

## 2. Product goal

Build a production-ready, self-contained Godot addon for large, smooth,
editable voxel terrain with seamless Transvoxel level-of-detail transitions.

The finished addon must support:

- deterministic regular and transition-cell meshing;
- streamed chunk loading and eviction;
- enforced 2:1 neighboring LOD relationships;
- multiple viewers;
- visual and collision terrain;
- runtime terrain edits;
- deterministic baking and pregeneration;
- versioned binary world, chunk, edit, and trace formats;
- bounded native worker scheduling;
- event-driven invalidation and completion;
- representative telemetry and profiling;
- headless and CPU-only operation;
- optional measured compute acceleration;
- stable Godot-facing APIs;
- reproducible builds and releases.

The goal is not merely a function that converts one density cell into
triangles. It is a maintainable terrain system that can be used by a real Godot
project for years.

## 3. Production strategy and backend policy

Publication replacement/retirement closure uses an immutable integer AABB index
and a directed dependency traversal. A replacement depends on overlapping old
coverage and unsafe old face neighbors; an old chunk depends on its overlapping
replacements. Each selected vertex is expanded once, and final membership is
sorted deterministically. This replaces repeated retirement scans without
relaxing complete authoritative coverage, 2:1 boundaries, collision readiness,
generation identity, or atomic publication. Independent all-pairs reference
tests and bounded locality measurements qualify this subsystem replacement;
GPU gameplay latency remains a separate, unqualified gate.
The GPU publication session retains these spatial indexes across queries while
exact key membership is unchanged. Readiness, generation, active content and
transition masks are never cached by this graph. Membership changes invalidate
the affected index; ending the GPU session releases its retained storage.

The optional staged runtime can activate exact interaction-focus lease keys
independently of distant background refinement through
`hierarchical_lod_viewer_activation_enabled` (default false). This changes work
admission, not meshing or authority: target-plan demand, visual readiness,
2:1 balancing, and atomic publication remain required. Viewer activation may
coarsen obsolete distant detail. Exact LOD0 lease keys are inserted before
balanced planning, and this mode projects only those foreground paths to target
resolution in one balanced request. Broad viewer topology advances through the
background staging path. It does not delay edit refinement for coarse edited
feedback. The legacy staged mode retains its content-first behavior. GPU
integration must measure movement and edit latency before qualifying this
opt-in policy.

The first production backend uses Eric Lengyel's official MIT-licensed
Transvoxel implementation and lookup data.

The implementation order is:

1. Integrate the official MIT files unchanged and isolate them.
2. Wrap them behind a project-owned typed meshing-backend interface.
3. Establish exact cell, chunk, seam, integration, editing, collision,
   streaming, soak, and performance baselines.
4. Use that backend in production until it is genuinely battle-tested.
5. Keep alternate backends behind the same contract.
6. Qualify the independent 0BSD backend later against the complete contract.
7. Replace the MIT backend only after the 0BSD backend passes the complete
   production suite.

The previous independent 0BSD work is not discarded, but it is not currently
claimed to be an exact drop-in replacement. Similar or usable output is not the
same as exact compatibility.

An exact replacement claim requires all of the following:

- all 256 regular cases pass;
- all 512 transition cases pass;
- all six transition orientations pass;
- winding, vertex placement, reuse behavior, normals, materials, and boundary
  ownership satisfy the declared backend contract;
- multi-cell and multi-chunk seam tests pass;
- edit-boundary and LOD-corner tests pass;
- visual and collision integration tests pass;
- deterministic hashes pass across supported builds and platforms;
- representative streaming, soak, and save/reload tests pass;
- no MIT-derived topology data is present in the 0BSD deliverable.

Passing isolated lookup-table or topology cases alone is insufficient.

Official regular and transition class IDs are indices used by the upstream
lookup implementation to group equivalent cases. They are not a public terrain
API and must not leak through the project-owned backend interface. An isolated
oracle harness may compare them as MIT-scoped implementation evidence, but the
0BSD package must not contain copied official mappings or fixtures derived from
them. The production contract is expressed through declared mesh, seam,
material, collision, determinism, and integration behavior.

The future original-vs-0BSD oracle must keep both sides license-isolated. The
MIT side loads the official implementation, the 0BSD side loads only the
independent backend, and a comparison controller supplies equivalent inputs
and compares normalized results. Only non-reconstructive aggregate reports,
hashes, and pass/fail summaries enter the 0BSD project. Full official outputs,
tables, class mappings, and derived fixtures remain on the MIT side.

## 4. License model

Project-owned code and documentation use 0BSD unless a file explicitly states
otherwise.

The official Transvoxel source, lookup data, and any substantial unchanged or
transformed portions derived from them remain MIT-licensed and must live only
under:

```text
addons/world_transvoxel/thirdparty/transvoxel_mit/
```

That directory must contain:

- the upstream MIT license and copyright notice;
- an `UPSTREAM.md` with the exact source URL and revision;
- unchanged upstream files or mechanically necessary packaging changes that
  are clearly documented;
- no project-owned runtime service implementation.

When the MIT backend is included, the distributed addon is a mixed-license
distribution: project code is 0BSD and the isolated official backend is MIT.
It must never be described as wholly 0BSD.

The following rules are mandatory:

- Do not copy MIT table data into 0BSD source.
- Do not convert MIT tables into generated 0BSD arrays.
- Do not place MIT-derived topology fixtures in 0BSD tests.
- Do not encode MIT lookup data into world or cache formats.
- Do not remove or weaken upstream notices.
- Do not copy Voxel Tools implementation code; it is an architecture
  reference only.
- Keep downloaded papers and repository checkouts under
  `references/downloaded/`, which remains untracked.
- Aggregate pass/fail counts, timing results, and non-reconstructive hashes may
  be retained as comparison reports.

Removing `addons/world_transvoxel/thirdparty/transvoxel_mit/` and disabling the
MIT backend must remove all Transvoxel MIT code and data from a distribution.

License-boundary validation is a release gate, not a documentation preference.

## 5. Repository and addon boundaries

All reusable terrain complexity belongs inside:

```text
addons/world_transvoxel/
```

The root directory:

```text
world_transvoxel/
```

is reserved for thin application usage, examples, scenes, project
configuration, and game-specific adapters. It must not contain meshing,
streaming, storage, edit indexing, collision generation, worker scheduling, or
GPU task ownership.

The addon must be usable by copying `addons/world_transvoxel/` into another
Godot project together with its documented native binaries and licenses.

Expected repository structure:

```text
addons/world_transvoxel/       self-contained native addon
world_transvoxel/              thin examples and application integration
docs/                          evidence and supporting design notes
references/                    pinned research manifest and local downloads
tests/                         cross-addon acceptance and integration tests
tools/                         bake, inspect, migrate, validate, benchmark
artifacts/                     ignored local build and test output
```

Expected addon structure:

```text
addons/world_transvoxel/
  plugin.cfg
  world_transvoxel.gdextension
  LICENSE_SCOPE.md
  bin/
  thirdparty/
    transvoxel_mit/
      LICENSE
      UPSTREAM.md
      Transvoxel.cpp
  src/
    api/
    core/
    backend/
    meshing/
    streaming/
    storage/
    edit/
    render/
    physics/
    gpu/
    bake/
    telemetry/
  shaders/
  editor/
  docs/
  tests/
```

Subsystem directories are ownership boundaries. They must not become cosmetic
folders around one central manager.

## 6. Technology baseline

Pinned baseline at the start of implementation:

```text
Godot primary             4.6.3-stable
Godot compatibility       4.7-stable
godot-cpp minimum         godot-4.5-stable
Zig                       0.16.0 stable
Python tooling            3.11 or newer
Official Transvoxel       51a494f03c5b024cd153b596bcc7152eb3cc93a6
Voxel Tools reference     595f52ee4e23203a865eeb981f115909f7aa92f4
```

Exact revisions are recorded in `references/manifest.json`.

Policy:

- Zig stable is the primary native compiler driver.
- Project-owned build, download, validation, and test automation uses Python;
  platform-specific shell scripting is not part of the toolchain.
- C++ is the native addon implementation language because Godot's supported
  binding is godot-cpp/GDExtension.
- The build must provide debug and optimized release configurations.
- The addon targets the lowest Godot minor version intentionally supported and
  is tested on the primary and compatibility versions.
- Toolchain updates are explicit commits with rebuild and compatibility test
  results. Versions do not float silently.
- Development snapshots are not production dependencies.
- Platform support is claimed only after that platform passes the build and
  runtime matrix.

The build entry points are Python programs that invoke SCons using godot-cpp's
supported binding generation, with Zig 0.16.0 as the C/C++ compiler, archiver,
and linker. Host operating system and architecture detection is centralized in
`scripts/wt_script_common.py`. The build does not modify vendored godot-cpp.

The initial validated platform is Windows x86-64. The native addon loads and
passes its M0 runtime test on Godot 4.6.3 and Godot 4.7. Other platforms are
not yet claimed.

Research and repository checks are reproduced with:

```console
python scripts/download_references.py
python tools/validate_repository.py
```

## 7. Language and execution boundaries

### Native C++

Native code owns:

- authoritative runtime terrain state;
- scalar and material sampling;
- regular and transition meshing;
- chunk and LOD records;
- scheduling, priorities, cancellation, and generation tokens;
- mesh and collision buffer preparation;
- edit commands and spatial invalidation;
- binary serialization and migration;
- baking and pregeneration;
- cache ownership;
- telemetry and traces;
- compute submission and result handling.

### GDScript

GDScript is limited to:

- editor plugin registration;
- scene wiring;
- example UI;
- non-critical project adapters;
- demonstrations and test scaffolding where timing is irrelevant.

GDScript must not own runtime density arrays, mesh construction loops, worker
queues, chunk dictionaries, LOD scheduling, binary codecs, or per-frame terrain
scans.

### Shaders and compute

Rendering shaders own visual material work.

Compute shaders may own large, naturally parallel batches when the complete
pipeline benefits. Candidate work includes:

- procedural density generation;
- broad edit-mask application;
- offline world and material baking;
- GPU-resident culling;
- future visual-only mesh paths whose output stays GPU-resident.

Compute is not automatically preferable for:

- small edits;
- ownership and scheduling decisions;
- collision meshes immediately needed on the CPU;
- work requiring synchronous frame-thread readback;
- counters polled every frame.

The deterministic CPU path remains supported even after compute is introduced.
Compute is an optional acceleration backend, not the only correct path.

## 8. Runtime architecture

The intended execution flow is:

```text
Godot scene and editor
        |
        v
WorldTransvoxelTerrain public facade
        |
        +-- native world state
        +-- native event bus
        +-- native stream scheduler and job graph
        +-- native data source
        +-- native meshing backend interface
        |      +-- official MIT CPU backend
        |      +-- future independent 0BSD backend
        |      +-- optional future GPU backend
        +-- native render preparation and apply queue
        +-- native collision preparation and apply queue
        +-- native storage and edit journal
        +-- native baking services
        +-- native telemetry and traces
        +-- optional dedicated GPU task service
```

Scene-tree and Godot object mutations occur on the main thread. Native workers
may perform sampling, meshing, compression, binary I/O, collision-face
generation, edit indexing, and scheduling calculations using immutable or
explicitly owned data.

Background threads must not mutate scene-tree objects.

## 9. Ownership rules

- World state owns chunk records and authoritative lifecycle state.
- Stream scheduler owns desired LOD, priority, and job issuance.
- Viewer service owns viewer snapshots, not scene nodes.
- Data source owns scalar and material sampling contracts.
- Meshing backend owns topology generation only.
- Render service owns visual resource creation and visibility application.
- Physics service owns collision resource creation and activation.
- Storage service owns schemas, files, caches, migrations, and corruption
  handling.
- Edit service owns typed commands, transactions, revisions, and spatial
  indexing.
- Bake service owns deterministic offline generation.
- GPU service owns RenderingDevice interaction, submission, synchronization,
  and readback.
- Telemetry service owns metrics and trace emission, not policy decisions.
- Application/game code owns buildings, vegetation, roads, gameplay spawning,
  AI, and unrelated world systems.

No subsystem may reach into another subsystem's mutable containers. Interaction
uses typed commands, immutable snapshots, handles, and events.

## 10. Event-driven scheduling

The runtime reacts to explicit state changes such as:

```text
viewer_changed
data_source_invalidated
edit_committed
chunk_requested
chunk_loaded
sampling_completed
meshing_completed
collision_completed
render_applied
chunk_eviction_requested
device_state_changed
```

Required scheduling behavior:

- Dirty queues contain only affected chunks.
- Each chunk generation has a token or revision.
- Late results are discarded when their token no longer matches.
- Jobs have typed inputs, typed outputs, priority, cancellation, and explicit
  result application.
- Worker queues, completion queues, render application, and collision
  application are bounded.
- Viewer movement changes priorities without rebuilding global state.
- Eviction cancels or invalidates obsolete work.
- Idle frames do not scan all active chunks to discover work.
- There is no generic `Dictionary request -> Dictionary result` worker API.
- No string-key payloads or `Variant` records cross native hot-path queues.

RenderingDevice work uses a dedicated native GPU task service. GPU submissions
and readbacks are batched where possible. The Godot frame thread never waits
for compute readback.

## 11. Chunk, LOD, and topology invariants

The implementation must define and test:

- chunk coordinates and negative-coordinate behavior;
- sample-grid dimensions and required padding;
- cell ownership at chunk boundaries;
- density sign convention and isovalue;
- material ownership and interpolation rules;
- vertex position precision;
- normal generation method;
- winding and coordinate handedness;
- transition-face orientation mapping;
- deterministic transition ownership;
- corner behavior where multiple LOD boundaries meet;
- edit invalidation across same-LOD and lower-LOD dependents;
- collision generation and thin-triangle policy.

These conventions must be written as backend-contract tests before streaming
depends on them.

Neighboring active chunks may differ by at most one LOD level. The scheduler
enforces this 2:1 relationship; meshing code must not attempt to repair an
invalid global LOD map after the fact.

Meshes are derived caches. The authoritative world is the data source or baked
sample data plus the ordered edit history.

## 12. Public API direction

The project does not want a huge public surface. It wants a small stable public
facade backed by multiple focused internal interfaces.

Planned Godot-facing classes:

### `WorldTransvoxelTerrain`

High-level `Node3D` facade:

- start and stop a world;
- attach configuration and data source;
- add and remove viewers;
- expose edit, query, bake, and telemetry capabilities;
- emit coarse lifecycle and error signals.

It does not expose internal queues, mutable chunk maps, RIDs, backend tables,
or worker state.

### `WorldTransvoxelConfig`

Versioned validated configuration resource:

- chunk dimensions;
- LOD count and distance policy;
- worker and frame-application budgets;
- collision policy;
- storage and cache limits;
- backend selection;
- feature flags.

Configuration is validated once and converted to immutable native runtime
configuration.

### `WorldTransvoxelDataSource`

Abstract scalar and material source:

- sample a bounded region;
- expose a deterministic content revision;
- support CPU/headless sampling;
- optionally advertise compute generation.

### `WorldTransvoxelEditAPI`

Command-oriented editing:

- begin transaction;
- append typed SDF or voxel commands;
- commit or cancel;
- report affected bounds and revision;
- support undo/redo where the selected storage policy permits it.

### `WorldTransvoxelQueryAPI`

Read-only queries:

- scalar and material samples;
- surface raycasts;
- visual and collision readiness;
- chunk, LOD, and diagnostic inspection.

### `WorldTransvoxelBakeAPI`

Editor and command-line entry point for deterministic binary baking,
validation, and conversion.

### `WorldTransvoxelTelemetry`

Documented metric snapshots and event/trace access.

Public API rules:

- No mutable internal containers are returned.
- No scene-tree group lookup is performed by native terrain internals.
- No gameplay manager reference exists inside the addon.
- No backend table type appears in public headers.
- Asynchronous work returns a handle and completes through typed state/events.
- Every persistent mutation advances a world revision.
- Every public and persistent API has a version policy.

Internal interfaces may be numerous because they are not compatibility
promises. Examples include meshing backend, sampler, chunk store, edit journal,
stream policy, render sink, collision sink, compression codec, checksum
provider, GPU service, and trace sink.

## 13. Binary data strategy

Binary formats are designed before baking and persistence code.

Format family:

```text
*.wtworld   immutable baked world bundle and section directory
*.wtchunk   independently cacheable chunk page
*.wtedit    append-only edit transaction journal
*.wttrace   optional binary telemetry trace
```

Common format properties:

```text
magic[8]
format_major:u16
format_minor:u16
header_size:u32
feature_flags:u64
source_revision:u64
directory_offset:u64
directory_size:u64
payload_hash[32]
```

Rules:

- little-endian on disk;
- fixed-width integer fields;
- explicit section offsets, bounds, and sizes;
- readers validate offsets before allocation;
- unknown optional sections can be skipped;
- unknown required features fail clearly;
- no serialized Godot `Variant`;
- no `store_var` persistence;
- no compiler-layout or pointer-sized structures;
- no filesystem timestamp as a content identity;
- independent section compression;
- checksums or hashes for corruption detection;
- deterministic writers and cross-platform readers;
- explicit migration tools for supported old versions.

Candidate world sections:

```text
META    dimensions, coordinates, generator identity
LOD0..N sample pages or source descriptors
BIOM    material or biome information
WATR    optional secondary field data
EDIT    compact initial edit history
INDX    spatial page directory
DEPS    source and toolchain manifest
```

Chunk pages must support direct range reads and contain a chunk key, LOD,
sample/material encoding, compressed and uncompressed sizes, content revision,
content hash, and payload.

The initial codec set must include `none`. Additional compression is accepted
only after deterministic, corruption, throughput, and random-access tests.

Edit records contain explicit schema version, command ID, world revision,
operation, shape, flags, material, bounds, typed payload, and record checksum.
Transactions commit atomically. Compaction creates a new authoritative
snapshot while retaining a migration audit.

Every baked artifact records source hashes, generator version, configuration
hash, backend revision, Godot/godot-cpp/Zig revisions, format version, and
deterministic output hash.

Binary files must remain backend-neutral and must never embed MIT lookup data.

## 14. Performance policy

Correctness and architecture come first, but inefficient foundations are not
accepted with a promise to optimize later.

Required design properties:

- native compact data structures;
- bounded caches and queues;
- reusable thread-local meshing scratch memory;
- batch allocation and transfer where appropriate;
- immutable worker inputs or explicit ownership transfer;
- no frame-thread blocking I/O;
- no frame-thread GPU readback;
- no scene-tree search in hot paths;
- no global mutable terrain dictionary;
- no per-frame full-world or full-chunk scan;
- separate visual and collision distance/readiness policy;
- asynchronous range-readable storage;
- deterministic CPU fallback;
- telemetry for queue depth, latency, memory, work cancellation, generation
  age, render readiness, collision readiness, and seam errors.

Performance claims require representative workloads:

- continuous active viewer movement;
- fast vehicle movement;
- teleportation;
- repeated edits while moving;
- multiple viewers;
- underground and vertical traversal;
- loading and eviction pressure;
- collision activation and deactivation;
- baked and procedural data sources.

Idle-only measurements do not prove production performance.

Numerical frame, queue, memory, and latency budgets must be recorded before the
relevant milestone exit. They must include test hardware, world settings,
duration, and percentile metrics. A feature is not called faster because an
isolated kernel is faster while end-to-end transfer or frame cost is worse.

## 15. Visual and collision separation

Visual topology and collision suitability are related but distinct contracts.

The system must separately validate:

- visual winding and normals;
- material attributes;
- cracks and duplicate surfaces;
- degenerate triangles;
- extremely thin triangles;
- collision-backend limits;
- collision activation distance;
- stale collision rejection after edits;
- bounded physics-resource application.

Collision may use a sanitized, simplified, or differently scheduled result as
long as it follows the authoritative surface contract and its differences are
documented and tested.

## 16. Testing strategy

Tests are built with the implementation, not added after it appears to work.

### Build and load tests

- debug and optimized native builds;
- addon load/unload;
- primary Godot baseline;
- one later supported Godot minor;
- headless startup;
- missing optional GPU capability;
- release package content and license audit.

### Backend contract tests

- all 256 regular cases;
- all 512 transition cases;
- all six transition orientations;
- empty and full cells;
- exact isovalue samples;
- mirrored and rotated configurations;
- deterministic output;
- invalid input handling;
- vertex bounds and index validity;
- winding and normal rules;
- material propagation;
- reusable scratch-state reset.

### Chunk and seam tests

- same-LOD neighboring chunks;
- each LOD transition direction;
- corners and edges with multiple transitions;
- negative chunk coordinates;
- world-origin boundaries;
- padded sampling;
- edit exactly on a chunk boundary;
- edit exactly on an LOD boundary;
- lower-LOD invalidation after edits;
- no open seams in deterministic galleries.

### Scheduling tests

- generation-token rejection;
- cancellation during each pipeline stage;
- priority changes after viewer movement;
- eviction with in-flight work;
- bounded queues;
- starvation prevention;
- multiple viewers;
- device loss and GPU fallback where applicable.

### Storage and editing tests

- deterministic bake hashes;
- direct range reads;
- save/load round trip;
- edit transaction atomicity;
- edit journal replay;
- compaction;
- migration;
- truncated files;
- corrupt offsets and sizes;
- invalid checksums;
- unknown optional and required features;
- cross-platform byte agreement where supported.

### Integration tests

- moving-viewer terrain readiness;
- collision readiness before contact;
- teleport;
- fast vehicle;
- underground travel;
- vertical world;
- continuous edits during movement;
- multiple viewers separating and converging;
- cache pressure and eviction;
- application shutdown with in-flight work.

### Soak and performance tests

- fixed-duration representative motion and edit workload;
- bounded memory;
- bounded queues;
- no increasing stale-work backlog;
- zero detected open seams;
- no invalid collision geometry;
- percentile frame and readiness latency budgets;
- reproducible trace and summary artifacts.

Every backend runs the same backend contract and integration suite. Backend-
specific tests are additional, never substitutes.

## 17. Source organization and coding standards

Naming:

```text
Public Godot classes    WorldTransvoxel*
Public source files     world_transvoxel_*
Internal C++ symbols    Wt*
Internal source files   wt_*
Shaders                 wt_*
Tests                   test_wt_*
Tools                   wt_*
```

Avoid generic names such as `manager`, `utils`, `common`, and `mesh_builder`
unless the name has a precise ownership qualifier.

Source-size policy:

```text
File type              Soft review     Hard CI limit
C++ implementation     600 lines       900 lines
C++ header             300 lines       450 lines
GDScript               250 lines       400 lines
Shader                  400 lines       650 lines
```

Vendored, generated table, and generated binding files are exempt only inside
explicitly excluded directories.

Crossing a soft limit requires a responsibility review. Files are split by
ownership and contract, not arbitrary line ranges.

Dependency direction:

```text
api -> services -> core
services -> backend interfaces
backend implementations -> backend interfaces + core
storage/render/physics/gpu do not depend on gameplay
thirdparty does not include project headers
```

Forbidden:

- circular service dependencies;
- cross-service singleton lookup;
- one terrain node owning every queue and cache;
- hidden global mutable state;
- worker access to scene-tree objects;
- unversioned persistent formats;
- modification of vendored upstream files;
- undocumented generated data;
- dynamic dictionary payloads in native hot paths;
- features merged without owner, tests, telemetry, and failure behavior.

Public classes receive GDExtension XML documentation. CI records source metrics
and enforces the hard limits.

## 18. Scope control

The addon owns terrain infrastructure. It does not become the whole game world.

Out of scope unless separately approved:

- building placement and building geometry;
- vegetation simulation;
- road gameplay systems;
- AI navigation policy;
- entity spawning;
- weather;
- general game save ownership;
- unrelated world-map UI;
- arbitrary gameplay-manager integration.

The addon may expose stable terrain queries, events, and adapters that these
systems consume. It does not own their state or behavior.

Water or secondary scalar fields may be supported by a deliberate extension
to the data and rendering contracts. They must not be inserted into the core
terrain controller as unrelated special cases.

New reconstruction papers and alternative algorithms are research candidates,
not reasons to reset the production architecture. They enter through an
explicit backend or offline-baker proposal with contract, license, and
performance analysis.

## 19. Lessons that must not be forgotten

The previous `world_marching_cubes` implementation demonstrated useful
capabilities, but its responsibilities accumulated into files that became hard
to reason about:

```text
chunk_manager.gd                         9,116 lines
mesh_builder.cpp                         3,229 lines
world_map_generator.gd                   3,009 lines
building_manager.gd                      2,985 lines
prefab_spawner.gd                        2,309 lines
```

The main script reached 356 functions, 109 exported settings, and 331 top-level
variables/constants while owning terrain, water, LOD, queues, rendering,
collision, edits, roads, vegetation/building integration, spawning, telemetry,
and scene lifecycle.

Do retain:

- native mesh construction;
- compact byte-oriented data;
- useful bulk compute work;
- collision and frame-application budgets;
- baking and pregeneration;
- persistent edit commands;
- real runtime telemetry.

Do not repeat:

- a universal chunk manager;
- polling every subsystem each frame;
- dictionary-based native jobs;
- synchronous GPU readback;
- storage formats grown from runtime objects;
- hundreds of loosely related exported settings;
- architecture documentation that no longer matches code.

Voxel Tools demonstrates useful separation of task execution, storage,
streaming, meshing, GPU work, and terrain integration. It also demonstrates
that mature systems can still grow 1,000-4,000-line source files. We adopt the
subsystem lessons, typed task model, dedicated GPU runner, batched readback,
thread-local scratch state, and explicit cancellation, while enforcing our own
source-size limits.

Recent Dual Contouring, AMR, McGrids, TetWeave, and power-diagram papers offer
valuable reconstruction or offline-baking ideas. None replaces the complete
editable, streamed, collision-enabled terrain engineering required here.

## 20. Finite implementation roadmap

Each milestone has an exit condition. Work does not continue through an
infinite sequence of undefined "next" tasks. A milestone is complete only when
its stated evidence exists.

### R0 - Research lock

Status: complete.

Deliverables:

- primary references downloaded and hashed;
- upstream revisions pinned;
- Marching Cubes architecture postmortem;
- Voxel Tools review;
- license boundary;
- architecture, API, binary format, coding standards, and finite roadmap;
- repository validator.

Exit: implementation direction is recorded before terrain code begins.

### M0 - Addon and official upstream baseline

Status: complete on 2026-06-20.

Deliverables:

- self-contained addon skeleton;
- exact official MIT source isolated under `thirdparty/transvoxel_mit`;
- upstream provenance and file-integrity checks;
- project-owned backend interface and minimal MIT adapter;
- pinned godot-cpp dependency;
- Zig-driven debug and optimized builds;
- GDExtension entry point;
- addon load test on Godot 4.6.3 and compatibility test on Godot 4.7;
- license and package-content validation;
- no terrain scene or streaming implementation.

Exit: the addon builds reproducibly, loads on both Godot baselines, and proves
that the official files and license boundary are intact.

### M1 - Exact native cell backend

Status: complete on 2026-06-20 for Windows x86-64.

Deliverables:

- typed backend contract;
- regular-cell meshing;
- transition-cell meshing;
- all regular and transition cases;
- all transition orientations;
- deterministic buffers and hashes;
- vertex/index/winding/material/normal validation;
- reusable thread-local scratch state;
- headless tests;
- no chunk streaming or scene ownership.

Exit: the official MIT backend passes the complete isolated native contract.

### M2 - Chunk and LOD ownership

Status: complete on 2026-06-20 for Windows x86-64.

Deliverables:

- native chunk keys, records, and lifecycle states;
- generation tokens and revisions;
- viewer snapshots;
- 2:1 LOD map enforcement;
- transition ownership rules;
- event-driven scheduler and typed job graph;
- cancellation and stale-result rejection;
- same-LOD, transition, edge, and corner seam galleries.

Exit: deterministic multi-chunk test worlds have zero detected open LOD seams
and no unbounded scheduling state.

### M3 - Godot render and collision integration

Status: complete on 2026-06-20 for Windows x86-64, Godot 4.6.3 and 4.7.

Deliverables:

- native mesh resource integration;
- bounded main-thread render apply queue;
- native collision preparation;
- thin/degenerate triangle handling;
- collision distance and readiness policy;
- bounded physics apply queue;
- visual/collision stale-result rejection;
- moving-viewer integration tests;
- telemetry for readiness and queue latency.

Exit: active movement tests pass without unbounded queues, invalid collisions,
or frame-thread readback.

### M4 - Storage, baking, and editing

Status: complete on 2026-06-21 for Windows x86-64, Godot 4.6.3 and 4.7.

Deliverables:

- versioned `wtworld`, `wtchunk`, `wtedit`, and optional `wttrace` schemas;
- deterministic command-line/editor baker;
- range-loadable pages;
- source and dependency manifests;
- typed edit transactions;
- spatial edit indexing and dependent-LOD invalidation;
- save/load, replay, compaction, corruption, and migration tests;
- format inspection and validation tools.

Exit: bake-load-edit-save-reload reproduces identical authoritative state and
all corruption cases fail safely.

### M5 - Streaming production baseline

Status: complete on 2026-06-21 for Windows x86-64, Godot 4.6.3 and 4.7.

Deliverables:

- multiple viewers;
- asynchronous storage and generation;
- bounded caches and eviction;
- explicit frame, memory, queue, and readiness budgets;
- teleport, fast-vehicle, underground, vertical-world, edit, and multi-viewer
  tests;
- representative metrics and binary traces;
- fixed-duration soak test;
- documented limits.

Exit: the representative workload meets its recorded budgets with bounded
memory and queues, zero detected seams, and correct collision readiness.

### M6 - Optional compute acceleration

Status: optional and deferred. It is not a production-release prerequisite.

Deliverables:

- measured CPU baseline retained;
- compute density and/or bake path selected from evidence;
- dedicated GPU task service;
- asynchronous transfer and batched readback strategy;
- device-loss and unsupported-device fallback;
- deterministic-equivalence policy;
- end-to-end benchmarks including transfer and application cost.

Exit: compute is enabled only for workloads where complete measured behavior
beats the CPU path without reducing correctness, headless support, or
maintainability.

### Production finish line

Status: complete on 2026-06-25 for World Transvoxel 1.0.5.

Required:

- official MIT backend remains the trusted reference;
- all build, backend, seam, scheduling, storage, edit, integration, soak, and
  performance gates pass;
- supported Godot and platform matrix is recorded;
- release addon installs into a clean Godot project;
- public API and operational limits are documented;
- licenses and provenance are complete;
- binary migrations are tested;
- release artifact is reproducible;
- a real terrain project can bake, load, stream, render, collide, edit, query,
  save, reload, and shut down correctly.

At this point the official MIT-backed addon is production-ready.

### Post-production 0BSD qualification

The independent 0BSD backend then enters the same backend and system suite.

Qualification order:

1. clean-room and license audit;
2. isolated exhaustive cell contract;
3. isolated official-oracle comparison;
4. chunk, seam, collision, and edit integration;
5. deterministic cross-build tests;
6. full streaming and soak workload;
7. production trial behind a backend switch;
8. only then, a decision about changing the default backend.

Until every required gate passes, documentation must say that exact official
compatibility is not proven.

## 21. Controlled open decisions

These are intentionally unresolved and must be decided at the listed
milestone, with tests and this charter updated. They are not permission for
unstructured experimentation.

| Decision | Must be fixed by | Required evidence |
| --- | --- | --- |
| First compressed storage codec | M4 resolved: none selected | RLE rejected; representative M5 codec benchmark required |
| Numerical production budgets | M5 resolved | representative hardware traces and fixed-duration soak |
| First compute-accelerated workload | M6 | end-to-end CPU/GPU comparison |

The default role of root `world_transvoxel/` is resolved: it is thin example
and application integration. Reconsidering that boundary requires an explicit
charter change.

Resolved in M0:

- build orchestration is SCons with Zig 0.16.0 providing compilation,
  archiving, and linking;
- the initial validated matrix is Windows x86-64 with Godot 4.6.3 and 4.7.

Resolved in M1:

- chunks contain 16 cells per axis, with padded scalar sampling for central
  differences;
- runtime density and isovalue are `float`, with negative density solid and
  equality outside;
- runtime material is categorical `uint16_t` selected from the solid endpoint;
- normals interpolate normalized world-space density gradients;
- project-owned automation uses Python 3.11+ entry points and shared
  cross-platform path/process/download helpers;
- scripts detect Windows, Linux, and macOS plus x86-64 and ARM64, while runtime
  platform support remains unclaimed until its complete matrix passes.

Resolved in M2:

- active chunks are non-overlapping octree leaves with a validated 2:1
  face-neighbor invariant;
- the coarser leaf exclusively owns transitions toward finer face neighbors;
- chunk output uses local `float` vertices with signed 64-bit integer origins;
- regular and transition source-edge interpolation is canonicalized on a
  `1/65536` base-unit local lattice;
- every sign-changing higher-LOD source edge is recursively resolved to the
  corresponding sign-changing unit edge, so regular and transition vertices
  use Lengyel's highest-detail surface-shift position;
- surviving full-face edges are split at quantized transition constraints
  before degenerate cleanup, preserving closure after surface shifting;
- simultaneous transition faces use progressive cross-face deformation so
  all signed edge and corner galleries close;
- native job queues, completion queues, records, viewers, sample caches, and
  mesh buffers have explicit construction-time bounds.

Resolved in M3:

- render payloads combine regular then face-ordered transition buffers and
  retain local positions, signed world origins, normals, and `uint16_t`
  materials;
- exact-zero-area visual triangles fail payload preparation;
- collision removes degenerate triangles and triangles with scale-independent
  squared shape ratio at or below `1e-12`;
- collision demand uses 96-unit activation and 128-unit deactivation
  hysteresis for the integration baseline;
- worker-safe render and collision submission queues are separate, bounded,
  and consumed only on the Godot main thread;
- stale generations are rejected before `ArrayMesh` or physics mutation;
- material IDs are transferred to Godot mesh `UV2.x` while authoritative
  payloads retain categorical `uint16_t` values;
- application telemetry records independent readiness, queue depth, stale
  work, failures, and total/maximum latency in application-frame ticks.

Resolved in M4:

- format schema 1 requires codec `none` and admits no compressed codec;
- chunk-page schema 1.1 stores sparse validated finest-edge `SHFT` records;
  edits invalidate those derived records, compaction rebuilds them from edited
  LOD0 truth, and procedural runtime jobs rebuild them from the base generator
  plus authoritative journal replay;
- dependency-free byte RLE was rejected because it expanded all locked page
  fixtures, while another codec requires representative M5 I/O and latency
  evidence.

Resolved in M5:

- LOD refinement uses a one-child-chunk exit hysteresis shell derived from the
  current desired plan; the production non-flat profile keeps three LOD0 child
  chunks of near-field refinement before its first coarse boundary;
- production collision hysteresis retains the M3 96-unit activation and
  128-unit deactivation defaults;
- production main-thread application defaults are four render and two
  collision resources per frame;
- a 32-render/16-collision burst has an eight-frame readiness bound;
- optimized Godot 4.6.3 and 4.7 application profiles are locked by real
  `ArrayMesh` and `ConcavePolygonShape3D` evidence;
- native trace schema 1 uses an 80-byte metadata section and fixed 128-byte
  checkpoint/final events with construction-time capacity;
- GPU capture saturation no longer prevents mixed visual/collision generations
  from completing CPU collision preparation: at most four
  immutable pre-mesh capture groups remain deferred, collision publication is
  recorded once per generation, and the same generation resumes GPU capture in
  priority order when capacity becomes available;
- the reference orchestration soak runs for 60 seconds, samples every 1,024
  simulated frames, admits at most 65,536 events, and requires zero runtime
  rejection/failure paths;
- M5's exit combines the soak with the locked decoded-page seam, native
  pipeline, and real Godot application evidence.

## 22. Change and review discipline

Every implementation change should answer:

1. Which subsystem owns this?
2. Which public or internal contract changes?
3. Which event starts the work?
4. Which state or resource owns the result?
5. How is cancellation or stale work handled?
6. Is persistent data affected?
7. What is the license provenance?
8. Which unit, contract, integration, or performance test proves it?
9. Which metric exposes failure or backlog?
10. Does the change move the current milestone toward its exit condition?

A change should be rejected or redesigned if it:

- creates a new universal manager;
- bypasses a subsystem boundary for convenience;
- introduces frame polling where an event is available;
- blocks the frame thread on I/O, workers, or GPU readback;
- creates an unbounded queue or cache;
- stores persistent data through Godot variants;
- puts performance-sensitive loops in GDScript;
- adds backend-specific data to the public API;
- mixes game systems into the addon;
- modifies vendored MIT files;
- imports license-incompatible code or data;
- exceeds a hard source limit;
- adds a feature without tests and observable failure behavior;
- changes the finish line to chase a new paper or technology.

Experiments are allowed in isolated branches or tools. They do not become the
production path until they satisfy the relevant contract, licensing, and
measurement requirements.

## 23. Immediate next work

Production qualification is complete. The next controlled phase is the
separate 0BSD backend qualification; it must not alter the default MIT backend
until every replacement gate passes.

Ordered work:

1. Complete: audit the Godot facade against the complete real workflow and
   record every missing bake, load, stream, render, collision, edit, query,
   save, reload, migration, and shutdown gate.
2. Complete: add schema-1 `WorldTransvoxelConfig` with native validation for
   all construction-time M5 limits and attach it to the terrain facade.
3. Complete: explicit stopped/starting/running/stopping/failed world lifecycle
   globalizes Godot paths and performs manifest startup/storage shutdown on a
   native control thread.
4. Complete at LOD0: connect non-empty baked pages through native viewer
   demand, page I/O, official MIT meshing, caches, and bounded Godot sinks.
5. Complete: add balanced multi-LOD viewer planning and drive transition pages
   through the same production coordinator.
6. Complete: build the root integration example and real baked-world scene
   using the official MIT backend.
7. Complete: expose immutable active-chunk/readiness query snapshots without
   leaking runtime containers.
8. Complete: add lifecycle-owned journal load/append and atomic public edit
   transactions.
9. Complete: replay journal commands into decoded dependency pages, replace
   affected loaded generations, and prove durable restart replay.
10. Complete: expose authoritative scalar/material queries and connect atomic
   side-by-side compaction and migration to the running lifecycle.
11. Complete: run a fixed-duration full-world Godot soak with real page I/O,
   official MIT meshing, render resources, collision resources, edits,
   save/reload, migration, public metrics, and clean shutdown from an isolated
   copied-addon project.
12. Complete: lock operational limits, the supported Godot/platform matrix,
   license/provenance contents, and reproducible release evidence.
13. Complete: mark the official MIT-backed addon
   production-ready and begin the separate 0BSD backend qualification.
14. Next: build the license-isolated 0BSD adapter and run the complete backend,
   seam, integration, persistence, and production qualification suite before
   considering any default-backend change.

M6 compute acceleration remains optional. Do not let it delay the measured CPU
production path unless an end-to-end workload proves that it is required.

The opt-in GPU interaction qualification records the authoritative journal
commit, dirty-page admission, collision preparation/publication, GPU dispatch,
readback, preparation, activation, first draw, queue age, regenerated cell
count, and transferred bytes on one bounded timeline. Instrumentation is
disabled by default and cannot alter scheduling or publication. Optimization
claims require matching traced and untraced deterministic routes.

Interactive edit admission preserves the order of accepted commands even when
several transactions are created from the same currently committed revision.
An edit that matches the committed revision when it enters the bounded runtime
queue is eligible for queue-order rebasing after an earlier admitted edit
commits. Rebasing regenerates the canonical transaction ID, command IDs, and
all revision fields before durable append. A transaction already stale when it
enters the queue remains stale and is rejected; journal order and replay
identity therefore remain authoritative.

### 23.1 GPU interaction derived-cache contract

Loaded LOD0 terrain in the interaction working set uses 8 by 8 by 8 regular
cell bricks with a one-sample edit halo. The CPU journal and decoded pages stay
authoritative. Each accepted edit generation carries bounded dirty world bounds
and an eight-bit regular-brick mask into the internal GPU capture; this metadata
does not change the public API or persistence format.

Candidate meshlets are written into a distinct arena slot while the previous
generation remains drawable. A one-workgroup GPU transaction validates every
candidate meshlet and changes all candidate and retirement activation flags as
one ordered cohort. A failed cohort keeps the old activation flags and old
coverage. The render thread may draw provisional arena views directly; CPU
counter availability cannot gate visual publication.

Only edited page-field ranges and configuration tokens are uploaded when the
dependency layout and transition mask match the previous generation. Clean
meshlets are copied on the device and dirty regular bricks are regenerated;
transition meshlets retain exact Transvoxel ownership. The asynchronous result
summary is exactly 20 bytes per candidate and exists only for telemetry,
failure cleanup, and reclamation. GPU fields, meshlets, candidate slots,
activation state, and journal-derived replay state remain capacity bounded.
Collision geometry remains CPU authoritative and cannot depend on GPU readback.

### 23.2 Incremental interaction collision contract

Loaded LOD0 collision is stored in eight independently replaceable 8-by-8-by-8
cell blocks. The edited-page dirty bounds already include the sampling and
ownership support required by the edit; collision block selection must not add
a second one-cell expansion across every block boundary. The collision builder
selects only blocks intersecting those authoritative bounds and merges them with the newest
cached same-key collision generation preceding the replacement, while the
physics sink retains every clean block. Internal application readiness is not a
cache identity source because frontend publication can advance independently.
Withdrawing collision demand evicts the cached collision generation at the same
desired-set transition that retires the physics shape, so later re-entry cannot
mistake a cache-only predecessor for a live block base.
The runtime records the exact collision generation active in the frontend
physics sink. Incremental preparation may use a cached predecessor only when it
matches that active generation. Prepared, staged, or cache-resident payloads do
not establish a patch base; a first edit without a live base publishes a full
collision payload from its already-complete CPU mesh.
For GPU-resident LOD0 edits with a matching live collision base, the CPU worker
extracts only the dirty 8-cubed collision blocks. It
does not build the complete regular chunk or transition mesh for collision;
visual extraction continues from the immutable GPU field capture. Jobs without
a matching live base build a complete authoritative regular CPU mesh and
publish a complete collision base; a partial block payload cannot safely create
a new physics body.
The reserved interactive collision worker never accepts background work.
Background mesh workers remain work-conserving: while interactive collision
patches are queued they assist that queue before taking background meshes. This
preserves dedicated capacity without serializing a multi-chunk edit burst.
Empty and nonempty collision patches publish as soon as the matching CPU
generation is ready; GPU capture capacity and visual activation cannot delay
them. A superseding generation cancels queued and active collision work and
discards stale completions.

A collision-required job always installs its terrain-completion callback,
including staged GPU visual replacements. The callback consumes the dedicated
CPU collision mesh before the visual placeholder completes. A GPU placeholder
is never accepted as collision geometry, and deferred visual completion never
repeats collision preparation after the collision branch completed early.

One configured mesh worker and one bounded queue lane are reserved for
foreground collision patches. Background storage, LOD, and visual work cannot
consume that worker. Completion becomes visible to the runtime before the
worker reports itself idle, and a queued collision publication records the
generation's collision branch as pending so readiness repair cannot schedule a
duplicate generation.
The runtime drains asynchronous completions before scheduler dispatch. While an
interactive collision mesh remains queued or executing, scheduler dispatch
admits only committed-edit-priority sample work and waits before preparing
another mesh. This prevents synchronous dependency
preparation for a cold background mesh from occupying the runtime thread after
the reserved worker has produced an authoritative collision result. Existing
background workers continue executing and all queues retain their configured
bounds. Runtime metrics expose queued and actively executing interaction meshes
separately; canceled queued jobs therefore cannot leave the dispatch gate set.

That pending-attempt marker deduplicates payload or remesh work only while the
desired set still requires collision. When viewer motion removes its collision
role while retaining the visual chunk, or removes the chunk while an explicit
collision viewer owns collision, readiness repair must publish the matching
negative requirement even if an earlier payload attempt remains recorded;
obsolete collision demand cannot hold a regional replacement or retirement
open. Frontend application of either
render or collision work notifies the runtime so deferred bounded queues continue
their readiness repair without another viewer event.

Outgoing visual chunks receive temporary collision promotion only when visual
viewers own collision demand. Once an explicit collision viewer exists, its
bounded LOD0 set is the sole collision authority; visual LOD retirement cannot
create collision work outside that set.

Runtime metrics report collision-repair passes, their publication/application/
pipeline gates, active deduplication attempts, obsolete-demand clears, duplicate
skips, desired collision count, and both bounded publication queue depths. A
stationary trace must therefore distinguish active work, capacity backpressure,
and a logically stranded readiness state.

While any application record has required collision debt, the runtime retries
the bounded repair pass after at most four milliseconds even if no producer
notification arrives. The retry becomes dormant as soon as every required
record holds its current collision generation. This guarantees stationary
fixed-point progress without a permanent polling loop; timed wakes are measured.

Implicit procedural hierarchies may end on dimensions that are not powers of
two. Such boundary parents refine to their declared in-world children; clipped
octants outside the descriptor are not treated as missing hierarchy. Explicit
page catalogs retain the strict complete-eight-child requirement. The LOD map
still enforces 2:1 balance across every declared neighboring chunk.

An incremental collision block patch requires a resident complete collision
base. If a moving edit reaches a newly streamed chunk before that base is
installed or after it was evicted, the already completed CPU mesh produces one
complete collision payload. Later generations resume bounded block replacement;
an unbased patch is never submitted to the physics sink.

Collision preparation never reserves or waits for GPU capture capacity. A
player-support collision job may retain a bounded immutable density-field
capture because that same generation is likely to become visual at arrival.
Retention completes the collision and scheduler branches before any GPU slot is
requested. Water-only and material-only journal revisions advance the installed
collision generation while preserving its unchanged shapes. Full regular
collision payloads are marked separately from transition-bearing payloads so
the bounded cache can satisfy regular-only readiness without rebuilding CPU
topology. GPU publication remains atomic and capacity bounded; collision
authority remains entirely on the CPU.

GPU-resident placeholder publications are geometry-free main-thread state
updates. They bypass the bounded CPU mesh apply queue and immediately establish
the matching external visual application record; generation, transition-mask,
and sink ownership checks still run before preparation. CPU mesh payloads retain
the existing bounded apply queue. This prevents background CPU render
publication from delaying a prepared interaction capture.

Storage keeps a bounded interaction lane for player-support, interaction-focus,
and committed-edit page dependencies. Background requests cannot consume its
reserved admission slots, and one additional worker consumes only interaction
requests. Both lanes publish into the same bounded completion ring and immutable
page cache. Queued background requests may be promoted in place; an active page
load remains one non-preemptible whole-page unit. Lane occupancy, starts,
promotions, rejections, and worker ownership are reported by storage metrics.

Cancelling or superseding a page-meshing generation releases each dependency
request that is still queued and has no remaining page-meshing owner. A request
already executing remains immutable and completes normally. Shared dependencies
remain queued until their last live meshing owner retires, and a request shared
with interaction warming is never cancelled through meshing ownership. Storage
and page-runtime metrics report queued dependency cancellations so interaction
lane reclamation is measurable without weakening queue capacity.

Interaction-focus leases warm missing LOD0 source pages and request those exact
keys as leaves of the next balanced visual plan. Forced leaves refine only their
ancestor paths; the planner adds required siblings and 2:1 neighbors, and
hierarchical publication retains old coverage until the balanced replacement is
ready. Each unique active focus key uses the reserved interaction storage lane
and is decoded into the bounded page cache when its immutable load completes.
Focus movement does not cancel an already executing immutable load, and ordinary
cache eviction reconstructs it from storage plus the authoritative journal.
Admissions, coalescing, cache hits, completions, topology refreshes, and
rejections are observable.

The production player projects each LOD0 support or current tool target into a
one-chunk three-dimensional interaction shell before updating its foreground
leases. Lease construction admits every exact path center first, then fills
nearest Manhattan halo layers across all centers, and stops at the native
64-key per-source capacity. A single focus therefore warms its complete 27-key
neighborhood. Shell loading uses the existing bounded interaction storage lane
and must not reject requests or block input, physics, or rendering. It changes
priority, derived-cache warmth, and exact target topology; balanced planning and
atomic regional publication remain authoritative.

Asynchronous meshing also preserves its reserved interaction worker after
admission. When an atomic GPU publication identifies a queued background mesh as
a required edit-cohort dependency and raises it to committed-edit priority, the
prepared immutable job moves to the bounded interaction queue. Reprioritization
must not leave that dependency waiting behind the background mesh frontier.
Already executing work remains non-preemptible, and a full interaction queue
retains the reprioritized job in the priority-ordered background queue.

A regional visibility publication accelerates missing replacement members with
a distinct coverage priority below player support and interaction focus. It
never promotes those members into the committed-edit mesh lane, and promotion is
monotonic so a member already belonging to a committed edit keeps its higher
priority. This prevents a large balanced-LOD publication cohort from tying with
and age-ordering ahead of the exact edit cohort while preserving old coverage
until the complete region is ready.

Committed-edit collision urgency is preserved through both bounded frontend
handoffs. An interaction-critical collision publication may bypass one retained
background render or collision publication at the start of a physics frame,
and the collision apply queue selects the oldest interaction-critical payload
before background payloads. FIFO order is preserved within each class, total
capacity is unchanged, stale-generation validation still occurs at apply time,
and the per-frame item and time budgets remain authoritative.

Player-support and interaction-focus leases confer the same collision urgency
on their active chunk keys. Their payload may advance the application record and
physics sink before a staged GPU visual replacement becomes ready. Earlier
same-key `ExpectChunk` and positive collision-requirement publications remain
mandatory ordering prerequisites; generation, requirement, sink, capacity, and
deadline checks are unchanged. Ordinary staged replacements still wait for
visual readiness, preserving atomic visual publication outside the interaction
region.

When a staged replacement produces render and collision payloads from the same
mesh completion, the collision publication is enqueued first. A saturated
visual publication ring must therefore not hold an already prepared collision
behind the matching render publication. Visual staging and regional activation
remain atomic; only authoritative collision readiness advances independently.

An authoritative empty collision result removes its physics body but retains a
bounded key-and-generation tombstone in the collision sink. Exact-generation
readiness, split visual/collision completion, and non-density generation carry
must treat that tombstone as applied collision. A later nonempty payload or
chunk retirement removes it, and world shutdown clears all tombstones. Runtime
metrics expose the tombstone count so empty terrain cannot create unreported
resource growth.

An incremental LOD0 edit publishes only the intersecting 8-cubed collision
blocks identified by its dirty-brick mask. The payload must retain that exact
mask through collision preparation and Godot application; unselected block
shapes stay active from the preceding generation. The resource cache stores a
merged complete generation when its prior complete payload remains resident,
and never replaces complete cached coverage with a partial patch. A partial
construction may extend an authoritative empty-generation tombstone by creating
the block body with empty unselected shapes. These rules prevent a local edit
from removing collision elsewhere in the same chunk.
Before publishing a partial patch, collision preparation compares its cached
predecessor generation with the generation already applied to the physics sink.
If they differ, an intervening queued generation may be superseded, so the
latest complete merged block set is published as an independently applicable
update. This prevents rapid edits in different blocks of one chunk from leaving
an old solid block in Godot after stale application entries are discarded.
`collision_payload_prepared` causal events report the exact dirty-block mask in
their status field so runtime evidence distinguishes partial and complete work.
The Godot authority node drains completed interaction-critical collision
publications again at physics priority -100, immediately before ordinary player
physics callbacks. This bounded drain uses the existing collision item and time
budgets, applies only collision payloads whose publication prerequisites have
already cleared, and leaves background render and collision work on the normal
idle-process path. Runtime metrics report physics-boundary calls, applied items,
total time, and maximum time.
GPU activation cohort queries keep full member inventories only for cohorts that
can commit. Waiting queries return the exact blocking member and scalar closure
counts; large diagnostic member samples and native phase timing are opt-in and
sampled by the controller. Waiting controller seeds are parked by that blocking
member's complete generation route and wake when it becomes prepared, with a
bounded fallback probe. This prevents diagnostic marshalling and repeated
unchanged regional closure queries from becoming frame work.
Page replay validates and applies each command in one pass over a temporary
page, adopting the page only after every resulting sample is finite. This keeps
command application atomic while avoiding a duplicate full-page SDF evaluation;
SDF support-band behavior outside geometric brush bounds remains authoritative.

Cached source pages remain immutable base data keyed by source revision. An edit
does not evict that base page: journal replay derives the requested world revision
when sampling the replacement. The runtime admits and starts committed-edit work
before applying a pending viewer plan, and interaction render/collision
publications are selected ahead of background publications. An independently
publishable edited generation may complete while a viewer plan is open; all
generation, revision, transition, and sink checks still apply independently to
its collision and visual branches.

A loaded GPU edit cohort may bypass an overlapping background LOD retirement
region only when every visual member of the committed world revision replaces
an already active identical chunk key with the same transition mask. The whole
revision activates in one cohort without retiring coverage. Missing active
coverage, an exact-key retirement, or any boundary-mask change uses the normal
regional publication graph; candidates whose masks are not prepared remain
pending. Collision readiness remains independent of this visual fast path.

The render activation cohort query exposes its immutable regional flag and
replacement/retirement counts as soon as the cohort graph is built, before
payload readiness. A controller may precommit a zero-retirement, same-layout
incremental edit only after it has gathered the exact replacement count from
one source/world revision, every required surface is present, and normal
preparation and request validation succeed for every member. The controller
commits the complete native cohort and queues one GPU activation command in the
same callback. A partially gathered revision remains pending. Render-thread
cohort validation must reject a missing, stale, mixed-revision, or failed
candidate while retaining old coverage. Transition-changing, streaming, and
LOD cohorts continue through the normal deferred publication graph.

Unsafe-face closure may pull retiring fine leaves immediately outside a new
coarse replacement's volume into the same atomic GPU cohort. Current desired
GPU leaves that remain active at their exact generation and transition mask are
admitted as retained cohort members when they overlap that retirement volume.
They participate in the authoritative coverage proof and immutable activation
inventory without new capture or activation. This permits the complete swap
while retaining continuous coverage; stale, retiring, mismatched, or
nonvisual leaves cannot satisfy the proof. A failed coverage query reports its
selected replacements and retirements, geometric coverage result, and first
non-authoritative member so a persistent publication wait is self-identifying.

After an external same-layout visual activation and independent collision
publication have both reached the exact application generation, their shared
frontend replacement marker is complete and is detached directly. It must not
seed or join a later regional LOD publication merely because unrelated viewer
retirements overlap the key. Detachment requires both required sinks to report
the exact generation; staged or stale work continues through the normal atomic
publication graph.

The production 16-request GPU capture queue reserves four admission slots for
incremental interaction generations. Background reservations, queued captures,
and in-flight captures share the remaining twelve-slot bound. A newer
background generation may replace older queued work in place without consuming
the interaction reserve. Interaction-only dequeue is an internal scheduling
operation; publication ordering and the public API remain unchanged.

Any GPU capture may consume its exact, prerequisite-safe resident
placeholder from the bounded publication queue when the capture reaches the
frontend after that frame's normal publication drain. The key, generation,
render kind, and placeholder source must all match, and an earlier
same-generation `ExpectChunk` prevents extraction. Native applies only that
lightweight visual expectation before capture admission. The matching collision
branch proceeds independently and does not gate visual placeholder admission;
visual-only replacement generations do not invent a collision dependency. A
missing application generation waits without occupying a GPU slot;
superseded generations are rejected by the existing readiness checks. Regional
publication remains unchanged.

For an asynchronous mesh job with an immutable pre-mesh capture reservation,
successful dispatch queues the geometry-free visual placeholder immediately.
The render callback may consume the captured field while CPU collision
extraction continues. Mesh completion may deliver the same placeholder again;
exact generation and transition checks make that delivery idempotent. A failed
dispatch or failed bounded publication admission remains a runtime failure and
cannot expose a candidate.

Decoded immutable pages cache the three static-water occupancy aggregates used
by GPU pre-mesh preparation: explicit inside, explicit outside, and occupied.
This derived metadata is absent from the persistence format and is rebuilt
during decode. Terrain edits invalidate it before publishing an edited page.
GPU-only visual preparation consumes valid aggregates instead of rescanning
every retained page sample; invalid or synthetic pages retain the exact scan as
a correctness fallback.

The native GPU identity exports whether the scheduler job is at or above
interaction-focus priority. The render-thread frontend uses that fact, together
with the incremental-edit flag, to select a reserved interaction dispatch lane;
it must not infer interaction locality from LOD or distance. GPU extraction
writes a per-slot generation-and-ticket completion token after all mesh writes.
That token releases only dispatch-lane capacity. Candidate geometry remains
provisional, old coverage remains active, and reclamation remains forbidden
until the existing cohort commit and asynchronous summary validation complete.
Background extraction has four outstanding completion tokens and interaction
extraction has eight; their queued admission bounds remain independent.

A visual replacement key without an application record is obsolete queue
residue, not an incomplete cohort member. Viewer supersession may remove the
record after the key moves from pending work into the ready regional queue, so
the frontend prunes both pending and ready staging queues and cohort selection
also excludes missing records. A live incomplete replacement always retains its
generation-bearing application record and continues to wait for exact visual
preparation. This prevents an unreachable member from holding later overlapping
LOD publication cohorts indefinitely without weakening atomic publication.
Face closure may add only fine neighbors accepted by the authoritative boundary
lookup. Other coordinates on the same four-child face are outside the live
publication set and cannot be invented as generation-zero cohort members.

The runtime drains all collision-viewer changes already queued at the start of
a planning pass into one immutable candidate collision-viewer set. It applies
one desired-set delta for that set, preserves each viewer's revision checks and
update/removal metrics, and requeues the complete batch in order under scheduler
backpressure. Current support, predictive support, and tool collision viewers
therefore share one collision overlay plan whenever they arrive together rather
than successively superseding the same page work.

The same immutable batching rule applies independently to queued visual-viewer
changes. A primary viewer and its predictive interaction shell must enter one
balanced LOD plan and one desired-set delta. They cannot create successive cuts
whose overlapping replacements become a growing regional publication cohort.
Collision and visual batches remain separate so collision demand keeps its
dedicated scheduling and publication path.

A capture may reach the frontend before its exact CPU application record.
Every later readiness query retries the generation-matched resident placeholder
as soon as that application generation exists, then re-reads application state
before answering. A waiting incremental capture remains bounded and prioritized but
cannot prevent the frontend from dequeuing other native captures in the same
frame. Retry status and the exact deferred identity remain observable.

LOD-map validation and balancing use exact dyadic ancestor and face-neighbor
lookups. They do not scan all pairs of active leaves. Staged planning builds one
immutable descendant-priority summary per target and reuses it for selection,
while preserving overlap rejection, deterministic face ownership, 2:1 balance,
transition masks, hysteresis, and atomic regional publication.

Broad visual LOD planning and staged topology projection are cooperatively
cancellable at viewer, desired-set, root/subtree, balance-pass,
demand-construction, coverage, coarsening, and refinement boundaries. A newly queued
authoritative edit cancels the local candidate plan, preserves the last accepted
topology and publications, and leaves the newest equivalent viewer event queued
for retry after the edit. Internal edit-retention refreshes restore their pending
flag; staging remains driven by its existing pending state. Cancellation is not
an event rejection. Runtime metrics expose its count and the maximum interval
from edit admission to planner yield.

Foreground projection is coverage-first. An empty or relocated visual plan
admits only its bounded coarsest target roots until every root has a visually
active covering leaf. LOD0 interaction projection begins afterward, under that
retained coarse cover. A cold start therefore cannot turn the entire fine
interaction shell into one inactive replacement graph, and a refinement never
removes the only already-published terrain beneath it.

Incremental LOD0 collision extraction has a dedicated regular-cell path. It
samples each selected 8-cubed block grid point once, derives a deterministic
cell gradient from the eight scalar corners, and emits canonical regular-cell
faces without render-material vertex deduplication. The authoritative triangle
finalizer still validates degeneracy, shared-edge consistency, and component
winding before block ownership and physics publication. Native parity tests
compare the expanded face sequence against the full-quality regular mesher for
planar, curved, and excavated terrain; this optimization cannot change visual
meshing, transition topology, collision capacity, or generation semantics.
When no live physics base exists, the same collision-specific path extracts all
eight blocks and publishes a complete replacement. A GPU-resident visual edit
must not run the full CPU render mesher merely to establish its first collision
base. The GPU capture retains the edit's original dirty-brick mask; widening the
CPU collision mask cannot widen visual regeneration.

An application-accepted collision generation is not a physics base while it is
still staged in the frontend. If a newer incremental edit can only find that
unpublished generation, it merges the patch into a complete eight-block payload
and applies the complete result. Partial blocks may be applied only over the
exact generation recorded as resident in the physics frontend. This prevents
rapid supersession from mixing collision blocks from different world revisions.

Interactive collision admission is measured independently from background mesh
traffic. Runtime metrics expose the last, total, and maximum queue wait for jobs
accepted into the interaction lane, in addition to aggregate mesh-worker wait.
The GPU capture interaction lane admits both incremental edit generations and
focus-priority visual generations. A viewer or transition refresh that supersedes
an edited chunk therefore cannot demote its final LOD0 generation behind cold
regional captures.
An exact-key replacement whose transition mask matches its active GPU predecessor
is independently publishable even when a viewer refresh produced it. It uses the
same bounded single-key activation path as an edit. Only replacements that change
coverage or a boundary mask join the wider atomic LOD cohort.
A same-revision edit cohort may also expose an internal surface in a chunk whose
previous GPU entry was proven empty. When every such member has transition mask
zero, the complete edit cohort publishes atomically without waiting for the cold
viewer region; non-edit insertions retain the regional coverage requirement.
Only incremental jobs that require collision contribute to these counters,
including jobs executed by a background worker assisting the interaction queue.
Road and burst-edit qualification must use the interaction counters when
attributing collision latency; aggregate wait alone cannot justify changing the
collision lane or its capacity.

Queued edit commands and the scheduler use bounded strict interleaving. After
committing one edit, each scheduler pass may dequeue one maximum-priority
interactive job while another edit remains queued; it may not dequeue background
work and it may not globally stop scheduler progress. This lets an earlier edit
reach sampling and the collision worker during sustained rapid input instead of
waiting for the command queue to become empty. The one-job bound preserves
intake time for viewer and collision-viewer control events.

An edit does not promote every intersecting loaded LOD to interaction priority.
Collision-required chunks and LOD0 chunks containing a brush center enter the
maximum interaction band. Other visual-only replacements retain their previously
accepted desired-set priority, so LOD1/LOD2 reconstruction cannot queue ahead of
LOD0 collision during a rapid edit burst. Every affected generation still uses
the same committed journal revision and atomic visual publication rules.

The frontend collision item budget limits background publication. An
interaction-critical collision payload may continue past that item count while
the configured collision-apply deadline still has time. Every payload is still
applied immediately after dequeue, and the first indivisible shape that crosses
the deadline ends the frame's collision drain. This uses reserved physics time
instead of forcing a two-item multi-chunk edit across additional physics frames;
it does not remove the deadline or enlarge any queue or resource capacity.

Authoritative edit replay mutates an owned candidate page in place and records
only the samples changed by the current command. If evaluation fails, those
samples are restored in reverse order before failure is returned. This preserves
command atomicity and all revision, sequence, material, water, and surface-shift
rules without copying the complete 19-cubed page for every intersecting journal
command. Page ownership remains private to the replay state, and the rollback
list is bounded by the fixed page sample count.

Runtime edit admission commits the validated encoded transaction to the bounded
in-memory journal before replacement planning begins. The identical encoded
segment then enters a single ordered persistence lane whose pending bytes cannot
exceed the journal byte capacity. Durable file flush and close run outside the
terrain dispatcher. Persistence failure is terminal and observable to the
runtime; no later edit may hide it. Snapshot creation and orderly runtime
shutdown are durability barriers and wait until every preceding segment has
reached stable storage. The synchronous journal-store append remains available
to offline tools and preserves its original durable-return contract.

Completed asynchronous meshes are consumed in fixed four-item dispatcher
slices rather than draining up to the active-chunk capacity in one pass. Within
each slice, completed interaction-collision work is selected before background
work. The completion queue capacity and generation checks are unchanged; work
left after a slice remains queued for the next runtime pass. This prevents a
burst of completed background meshes from monopolizing edit admission while
still allowing twelve completions across the three normal completion points in
one dispatcher iteration.

Storage workers validate and decode immutable pages before publishing bounded
completions. Runtime completion admission only installs the encoded and decoded
objects into the bounded cache and pins waiting dependencies; cold whole-page
decode cannot occupy the world-runtime thread or delay a newly submitted edit.
The worker also carries the verified content hash and decoded metadata, so the
runtime does not reopen or rehash the same immutable page during admission.
Storage completions are admitted in fixed two-item slices.
If an edit arrives during that slice, the dispatcher commits it before resuming
background loading records. Remaining storage completions stay in the existing
	bounded completion ring and retain their original ownership and validation;
	this changes dispatcher residency rather than storage capacity or page order.

Foreground priority overlays batch generation-checked scheduler priority
changes. The bounded job queue is scanned once and sorted once per overlay,
instead of scanning and sorting the whole queue once per changed chunk. This
keeps fast viewer/interaction lease updates from creating quadratic runtime
thread stalls while preserving the existing priority order and trace records.

An active `InteractionFocus` lease is also a bounded visual LOD0 topology
request. A cold focus page is omitted while unavailable, but successful storage
completion must rearm foreground topology refresh while that exact key remains
leased. Visual admission therefore cannot wait for an unrelated later viewer,
collision, or edit event. Superseded or released lease keys do not rearm it.

An active focus refresh projects its exact ray/tool LOD0 leaves from the currently
accepted visual cut. It splits only containing ancestors and the neighbors
required for 2:1 balance, so admission cost and replacement scope follow the
bounded interaction shell instead of the full moving-viewer target. The result
still passes through coverage-first staging: active coarse leaves remain visible
until the complete local descendant cut is ready for atomic publication. A
released shell returns to ordinary viewer planning. Capacity failure retains the
accepted cut and is reported; it cannot partially publish or drop coverage.
The native storage lane independently expands those exact keys to a bounded
one-chunk sampling/warm halo. Halo pages never become visual topology demands
merely because they were prefetched, so prewarming cannot inflate the atomic GPU
replacement cohort.

GPU publication does not connect equal-LOD cold candidates into one cohort
when neither shared face carries a transition. Their deterministic boundary
samples permit independent publication; parent/child replacement coverage and
all transition-bearing faces remain atomic. An edit additionally distinguishes
chunks whose owned cells intersect the command from chunks reached only through
the required sampling halo. Among owned-cell replacements, only chunks belonging
to the currently activated visual topology form the atomic interaction cohort.
Loaded candidate LOD variants still regenerate at the same journal revision, but
remain background replacements until their own topology is activated. They cannot
join or delay the visible edit cohort. This prevents an in-progress LOD transition
from coupling a visible LOD0 edit to an inactive overlapping LOD generation while
preserving atomic publication across every active chunk touched by the command.
Halo-only chunks likewise cannot delay publication of unchanged owned topology.
Internal cohort inspection reports the bounded reason and first key whenever
the same-layout edit path is unavailable.

## 24. Final definition of success

Success is a maintainable native Godot terrain addon, not merely generated
triangles and not merely a license claim.

The result is successful when:

- users can install the addon predictably;
- the official MIT backend produces trusted Transvoxel terrain;
- the addon streams, renders, collides, edits, bakes, saves, and reloads real
  worlds;
- behavior is deterministic where declared;
- runtime work is bounded, event-driven, observable, and measurable;
- native code owns efficiency-sensitive work;
- compute acceleration is evidence-based and optional;
- binary formats are explicit and migratable;
- source ownership remains clear and files remain maintainable;
- game-specific systems stay outside the addon;
- licenses remain accurately separated;
- every production claim is backed by a repeatable test or trace;
- the 0BSD backend is described honestly until it passes the complete
  replacement qualification.

That is the finish line. Work that does not contribute to it should not steer
the project.
### Player-support collision lane

Collision mesh preparation carrying `kWtPlayerSupportPriority` uses the same
bounded reserved worker lane as committed edit collision patches. This keeps
the current and predictive player-support shell independent of background GPU
field capture and LOD meshing. Interaction-focus visual work and distant
collision work remain on the bounded general worker queue.

### Player-support role promotion

A player-support collision generation that retained an immutable pre-mesh field keeps
its ownership when visual demand reaches the same key. Promotion during CPU
meshing, after mesh completion, or after scheduler readiness publishes the
matching geometry-free GPU placeholder without creating a successor generation.
The completed collision branch is never repeated. If bounded GPU admission was
unavailable when collision began, visual promotion follows the normal bounded
remesh path while the already published collision remains authoritative.

Retained field data occupies no GPU admission slot and is invisible while a
chunk remains collision-only. GPU capture is admitted only after matching visual
demand arrives.
Superseding or removing the generation cancels its visual and collision outputs
under the existing generation checks. Static-water capture follows the same
immutable retained pages so later visual promotion cannot mix revisions.

### Generation-owned edit deltas

Every runtime edit replacement generation owns an immutable delta descriptor:
the exact union bounds and 8-cubed regular-brick mask of commands from the
transaction that created that generation. The descriptor travels unchanged
through scheduler sampling and meshing jobs. Superseding a generation therefore
supersedes its work description together with its outputs.

Complete journal replay remains the authority for density, material, water, and
surface-shift values. Historical dirty bounds may still drive a required
surface-shift rebuild, but they may not select collision blocks or GPU regular
bricks for the current edit. Halo-only dependency generations carry a valid
zero-brick delta so their revision and transition data can advance without
inventing owned collision work. Runtime metrics report exact-delta chunks,
dirty-block totals and maxima, and cases where a cumulative historical mask was
prevented from expanding current derived work.

Loaded edited pages also form a bounded native derived cache. Each entry is
identified by chunk key, source revision, and its latest world revision. A
successor generation starts from the newest entry no newer than its requested
revision and replays only the remaining journal range. The cached page includes
the authoritative density, material, water, and surface-shift state, so
unchanged surface-shift records are retained from that revision rather than
from the original baked page. Cache entries use least-recently-used eviction,
are capped by both configured decoded-page entry and byte capacities, and are
discarded with the runtime. Journal and persisted pages remain authoritative; eviction
changes latency only. Runtime metrics expose hits, misses, updates, evictions,
entry capacity, and logical resident bytes.
### Physics-boundary collision publication

The Godot frontend must never consume a collision payload from `_process()`.
The runtime exposes a non-collision publication pop for render and bookkeeping
work, while `_physics_process()` drains every collision payload and prefers
interaction-critical payloads. This keeps all physics shape mutation on one
engine boundary. Visual publication remains independent and may complete first;
the previous authoritative collision remains active until the matching
generation is ready. Queue capacity, collision apply budgets, generation checks,
and stale-payload rejection remain unchanged.

Collision application is exclusive to the physics callback. The render callback
submits and applies visual work with a zero collision budget. At each physics
boundary, already-submitted collision work resumes before another runtime
publication is accepted, so a deadline deferral cannot migrate collision work to
the render callback or strand it between the two callbacks.

The collision readiness repair marker belongs to collision work until residency
is confirmed. A rejected or invalid frontend submission releases it immediately;
an accepted submission retains it until the physics sink reports the matching
active `(chunk, generation)`. Queue and pipeline occupancy suppress duplicate
repair while work is real, without opening a consume-to-residency race that can
rebuild the same collision every runtime wake. An historical failed attempt
cannot leave an idle collision-required chunk permanently unready.

### Same-generation collision refresh

Collision content validity is owned by the authoritative world revision rather
than by the visual topology generation. Each application record retains both
the physical collision payload generation and the world revision represented by
that payload. A visual-only successor may preserve and publish the existing
collision when its world revision is unchanged. A journal edit advances the
world revision immediately, making the retained physical shape non-current until
the matching collision branch publishes at the physics boundary. Runtime repair,
readiness, and diagnostics use this content-revision test, so LOD/viewer churn
cannot cause redundant collision extraction or movement waits.

Scheduler admission and mesh dispatch use the same content-revision predicate.
A mixed visual/collision record whose collision already represents its current
world revision dispatches as visual-only: it does not bypass saturated GPU
capture capacity, select a collision patch base, extract collision blocks, or
publish another physics payload. Initial load, collision-role promotion, and a
journal revision change make the predicate true and retain the authoritative CPU
collision branch. Visual generation tokens alone never request collision work.
After the physics sink installs or publishes a shape, the frontend reports its
generation and authoritative world revision back to the worker-side application
record. A same-revision successor may acknowledge a still-active predecessor
generation; a stale revision may not. Collision removal clears that residency.

Collision demand entering an already visual GPU chunk refreshes CPU collision
topology inside the existing `(chunk, generation, world revision)` identity.
The scheduler reopens that record for bounded sampling and CPU meshing without
allocating a successor generation. The application marks the work as a
collision-only refresh, so scheduler admission reserves no GPU capture slot and
mesh completion emits no render payload. The active visual, its LOD ownership,
and its transition state remain unchanged.

Role promotion is absorbed by an existing mesh job only before that job captures
its collision flag. A visual promotion may additionally reuse a retained,
immutable pre-mesh GPU field capture. A later collision promotion uses the
refresh path. A later edit or topology replacement still creates a new generation and
therefore cancels stale refresh output through the normal generation and world
revision checks. CPU collision remains authoritative and is published only at
the physics boundary.

If an immutable collision payload for the active generation remains cached after
collision deactivation, promotion republishes that payload directly. It must not
recompute and reinsert a different payload under the same identity. Runtime
completion also reuses an existing generation-matched collision payload when a
duplicate completion arrives. GPU resident placeholder publication is claimed
once in the application record and enforced at the publication queue boundary,
so dispatch, cached promotion, and CPU completion cannot publish the same visual
generation more than once.

Runtime
metrics expose the terminal runtime status plus the exact terrain mesh completion
failure substage and status so an automated route reports its native failure
instead of only reporting rejected viewer calls.

### Continuous GPU residency reconciliation

GPU residency reclamation is incremental after a viewer plan has published its
complete desired-state snapshot. An active GPU chunk is protected while its key
belongs to a pending or ready replacement, chunk retirement, or render
retirement. Other active chunks are reconciled against their exact application
generation and may retire while unrelated streaming work remains in flight.

An open viewer plan retains all existing GPU coverage because its replacement
and retirement inventory is not complete yet. Closing the plan changes the gate
from global quiescence to per-key protection; it does not weaken connected
Transvoxel region coverage, 2:1 balance, generation checks, or atomic visual
publication. Metrics distinguish general coverage staging from the number of
identities retained by an unfinished region.

### Reserved collision-edit execution lane

The reserved meshing worker admits only incremental edits that require
collision. Cold streaming and predictive player-support collision jobs keep
their numeric priority on the background workers and cannot enter or be
promoted into this lane. Background workers may help drain queued edit work,
but the reserved worker never starts whole-page streaming work. This makes the
edit-to-physics start bound independent of cold streaming backlog while keeping
the CPU collision mesh authoritative.

An incremental collision mesh enters a separate bounded completion queue as
soon as its dirty blocks finish, before the mixed worker job continues
static-water or visual completion. The runtime drains this queue before normal
mesh completions and generation-checks it through the existing collision
callback. The final mixed completion records that its collision branch already
completed and cannot repeat it. If the early queue is full, the worker retains
the collision for the normal completion path, preserving bounded memory and
eventual progress.

LOD0 collision extraction samples the selected 8-cubed block union into a
fixed 17-cubed dense lattice owned by worker scratch. Adjacent dirty blocks
share their boundary samples, and cell extraction uses direct indexing while
retaining canonical chunk traversal order. This removes per-corner hash-table
work without changing scalar authority, regular-cell topology, face ordering,
the eight independently replaceable physics shapes, or atomic publication.

### Bounded interaction visual replacement regions

An interaction-priority incremental edit may not inherit the complete global
GPU replacement graph. When its exact chunk is covered by an active ancestor
that is already scheduled for visual retirement, cohort selection uses that
ancestor as an immutable spatial boundary. Only replacement and active
retirement keys contained by the ancestor participate in the atomic swap.

The active ancestor remains visible until every selected descendant and
transition boundary is prepared. Missing active coverage, a missing matching
retirement, an incomplete authoritative partition, or stale generation state
falls back to the general publication graph. This isolation changes scheduling
scope only; it does not permit overlapping publication, partial coverage, mixed
world revisions, or early retirement. GPU request identities carry the internal
incremental-edit and interaction-priority classification through preflight and
activation so both phases select the same region.
