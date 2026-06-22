# Production Qualification Audit

Date: 2026-06-22

Status: PQ2 complete; PQ3 is the active finite gate

## Result

M0 through M5 plus PQ0/PQ1 now provide a tested terrain streaming path.
`WorldTransvoxelTerrain` connects real viewer events, global balanced 2:1 LOD
planning, asynchronous baked-page I/O, official MIT regular/transition
meshing, bounded render/collision application, and shutdown.

The root `world_transvoxel/` scene proves that path as thin application code.
The addon is still not production-ready because clean-install soak and release
qualification remain explicit PQ3-PQ4 work.

## Workflow audit

| Capability | Proven component state | Production integration state |
| --- | --- | --- |
| install addon | debug/release binaries load on Godot 4.6.3 and 4.7 | clean copied-addon project not yet proven |
| configure runtime | M5 limits and defaults are locked | explicit schema-1 `WorldTransvoxelConfig` now implemented and attached |
| bake world | deterministic CLI and editor path complete | root example preparation invokes the public native bake path |
| load world | manifest/page readers and corruption checks complete | root scene starts a validated 28-page hierarchical manifest |
| stream chunks | global balanced viewer planning, scheduler, async I/O, caches, and official MIT meshing complete | root transform events prove real movement |
| render/collide | real regular and transition baked pages drive bounded Godot sinks | root scene audit passes both supported engines/builds |
| edit world | transaction, spatial invalidation, journal, replay, and replacement complete | typed public transactions now durably append and replace affected loaded generations |
| query world | native component inspection exists | readiness snapshots plus exact asynchronous scalar/material queries complete |
| save/reload | journal and compaction round trips complete | committed edits reload/replay and public side-by-side compaction reopens equivalently |
| migrate | native/Python migration tooling complete | schema-1.0 load, public migration, and current-schema reopen complete |
| telemetry | bounded binary trace and 60-second orchestration soak complete | not exposed through a public runtime capability |
| shutdown | queued Godot application teardown is tested | PQ1 root scene stops with zero retained resources |

The addon version was stale at `0.5.0-m4`; the configuration foundation moves
it to `0.6.0-m5`, matching the completed milestone without claiming a
production release.

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

Status: active.

- copy only the distributable addon and required notices into a clean Godot
  project;
- run the complete real-world workflow for a fixed duration;
- lock memory, queue, frame, collision, seam, persistence, and shutdown
  evidence.

Exit: clean-install and full-world soak gates pass on the supported engine
matrix.

### PQ4 - Release qualification

- document public API and operational limits;
- lock supported Godot/platform matrix;
- reproduce and compare release artifacts;
- verify license/provenance contents in the installed addon.

Exit: the official MIT-backed addon is production-ready.

M6 compute acceleration is optional and outside these required gates. The
independent 0BSD backend enters its separate qualification only after PQ4.
