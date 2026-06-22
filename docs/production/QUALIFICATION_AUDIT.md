# Production Qualification Audit

Date: 2026-06-22

Status: PQ4 complete; World Transvoxel 1.0.1 corrective release qualified

## Result

M0 through M5 plus PQ0-PQ3 now provide a clean-installed, tested terrain
streaming and persistence path.
`WorldTransvoxelTerrain` connects real viewer events, global balanced 2:1 LOD
planning, asynchronous baked-page I/O, official MIT regular/transition
meshing, bounded render/collision application, and shutdown.

The root `world_transvoxel/` scene proves that path as thin application code,
and the isolated PQ3 project proves that the addon does not depend on that
repository integration. PQ4 proves that the exact installed 1.0.1 directory is
reproducible, self-contained, licensed, documented, and passes that same
matrix. The official MIT-backed addon is production-ready within its recorded
support limits.

Version 1.0.0 is withdrawn. The corrective 1.0.1 qualification adds explicit
Godot clockwise render winding, matching one-sided collision winding, outside
ray proof, canonical primary/secondary transition deformation, convex refined
corner closure, post-deformation outward-winding checks, and full-world
mixed-LOD manifold validation.

## Workflow audit

| Capability | Proven component state | Production integration state |
| --- | --- | --- |
| install addon | debug/release binaries load on Godot 4.6.3 and 4.7 | isolated copied-addon import and 15-second workflow matrix pass |
| configure runtime | M5 limits and defaults are locked | explicit schema-1 `WorldTransvoxelConfig` now implemented and attached |
| bake world | deterministic CLI and editor path complete | root example preparation invokes the public native bake path |
| load world | manifest/page readers and corruption checks complete | root scene starts a validated 28-page hierarchical manifest |
| stream chunks | global balanced viewer planning, scheduler, async I/O, caches, and official MIT meshing complete | root transform events prove real movement |
| render/collide | real regular and transition baked pages drive bounded Godot sinks | root scene audit passes both supported engines/builds |
| edit world | transaction, spatial invalidation, journal, replay, and replacement complete | typed public transactions now durably append and replace affected loaded generations |
| query world | native component inspection exists | readiness snapshots plus exact asynchronous scalar/material queries complete |
| save/reload | journal and compaction round trips complete | committed edits reload/replay and public side-by-side compaction reopens equivalently |
| migrate | native/Python migration tooling complete | schema-1.0 load, public migration, and current-schema reopen complete |
| telemetry | bounded binary trace and 60-second orchestration soak complete | immutable public runtime/application metrics snapshot is checked by PQ3 |
| shutdown | queued Godot application teardown is tested | source, compacted, and migrated clean-project worlds stop with zero retained resources |

The addon version is `1.0.1` with milestone identity `PQ4`.

## Finite qualification gates

### PQ0 - Configuration and lifecycle

- complete: immutable-on-start schema-1 runtime configuration resource;
- complete: native and Godot validation on both supported engines;
- complete: explicit stopped/starting/running/stopping/failed lifecycle;
- complete: validated manifest path and asynchronous startup/shutdown
  ownership.

Exit: passed. A terrain node starts, reports, fails, restarts, and stops one
validated baked manifest through production storage ownership without leaked
workers/resources.

### PQ1 - Read-only real world

Status: complete on 2026-06-22.

- complete: connect native viewer events to desired-set, scheduler, async page
  I/O, official MIT meshing, resource caches, and bounded Godot sinks;
- complete at LOD0: prove movement, underground, vertical, and multi-viewer
  operation in a real non-empty baked world;
- complete: globally merge multiple viewers into bounded non-overlapping 2:1
  leaves, assign coarse transition ownership, and remesh changed masks;
- complete: stream and apply real baked LOD1/LOD0 transition plans in native
  debug/release and Godot 4.6.3/4.7 debug/release tests;
- complete: provide the thin root `world_transvoxel/` example and close its
  real-scene audit on the supported engine/build matrix.

Exit: passed. The example streams, renders, and collides against a real baked
world with bounded queues and correct shutdown.

### PQ2 - Editing, query, and persistence

Status: complete on 2026-06-22.

- complete: expose typed immutable active-chunk/readiness snapshots;
- complete: own journal load/append in the lifecycle and expose atomic public
  edit transactions;
- complete: connect durable edits to invalidation, replacement, edited-page
  replay, readiness recovery, stale rejection, and restart reload;
- complete: expose exact authoritative scalar/material queries that reject
  invalid, absent, corrupt, and disagreeing overlapping samples;
- complete: compact edited worlds and migrate empty-journal worlds through the
  ordered application lifecycle into atomically published side-by-side
  directories;
- complete: reopen compacted and migrated snapshots and prove identical
  authoritative values.

Exit: a real application edit survives save/reload and returns identical
authoritative query results.

### PQ3 - Clean install and full-world soak

Status: complete on 2026-06-22.

- complete: copy the distributable addon and required notices into a generated
  project with no repository application/build/tool dependencies;
- complete: perform first-run Godot import and verify the copied GDExtension
  cache;
- complete: query all 28 pages, stream the complete movement pattern through
  real regular/transition meshing, render, and collision ownership;
- complete: commit edits from initial world revision zero and verify ordered
  authoritative queries;
- complete: compact, stop, reopen, migrate, stop, reopen, and prove equivalent
  authoritative values;
- complete: lock 15-second-per-case memory, queue, frame, latency, transition,
  persistence, and clean-shutdown evidence on Godot 4.6.3/4.7 with debug and
  release binaries.

Exit: passed. Reference evidence is
`docs/evidence/pq3_clean_install_soak_windows_x86_64.json`.

### PQ4 - Release qualification

Status: complete on 2026-06-22.

- complete: ship public API and operational-limit documentation inside the
  addon;
- complete: lock Windows x86-64 with Godot 4.6.3/4.7 and debug/release
  runtime binaries;
- complete: perform two clean builds and compare complete directory artifacts
  byte-for-byte;
- complete: verify license/provenance contents in the installed addon;
- complete: execute packaged bake/storage tools outside the repository;
- complete: rerun the full PQ3 matrix against the exact release directory.

Exit: passed. Reference evidence is
`docs/evidence/pq4_release_windows_x86_64.json`; the ignored local release is
`artifacts/release/world-transvoxel-1.0.1-windows-x86_64/`.

M6 compute acceleration is optional and outside these required gates. The
independent 0BSD backend may now enter its separate qualification.
