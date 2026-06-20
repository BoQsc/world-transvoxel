# World Transvoxel Implementation Charter

Status: canonical project direction

Last reviewed: 2026-06-20

Current state: M3 render/collision integration complete; M4 storage/baking/editing is next

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

Status: next.

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
| First compressed storage codec | M4 | determinism and throughput tests |
| Numerical production budgets | M5 | representative hardware traces |
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
  hysteresis for the integration baseline, while production distances remain
  an M5 budget decision;
- worker-safe render and collision submission queues are separate, bounded,
  and consumed only on the Godot main thread;
- stale generations are rejected before `ArrayMesh` or physics mutation;
- material IDs are transferred to Godot mesh `UV2.x` while authoritative
  payloads retain categorical `uint16_t` values;
- application telemetry records independent readiness, queue depth, stale
  work, failures, and total/maximum latency in application-frame ticks.

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

The next and only active milestone is M4.

Ordered work:

1. Freeze versioned `wtworld`, `wtchunk`, `wtedit`, and optional `wttrace`
   headers, section directories, feature flags, checksums, and size limits.
2. Implement bounded little-endian readers and deterministic writers with the
   uncompressed `none` codec as the required baseline.
3. Add direct range-readable chunk pages and explicit source/dependency
   manifests without embedding backend lookup data.
4. Build deterministic command-line and editor baking entry points using the
   native sampling and chunk contracts.
5. Define typed edit commands, atomic transactions, world revisions, and a
   spatial index that invalidates affected same-LOD and dependent lower-LOD
   chunks.
6. Implement edit journal replay, compaction, save/load, and backend-neutral
   authoritative state reconstruction.
7. Add truncation, corrupt offset/size/hash, unknown feature, migration, and
   deterministic byte-agreement tests.
8. Record exact M4 evidence and only then mark M4 complete.

Do not begin production streaming, representative soak budgets, or compute
acceleration during M4.

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
