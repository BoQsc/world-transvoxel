# Production Qualification Audit

Date: 2026-06-21

Status: current production boundary and finite remaining plan

## Result

M0 through M5 produced tested native components, but the addon is not yet a
production terrain system. `WorldTransvoxelTerrain` currently owns the Godot
render/collision application queues and test fixtures; it does not yet connect
viewer demand, page meshing, edits, queries, or persistence into the now
implemented manifest lifecycle.

The repository also has no root `world_transvoxel/` example/application
directory. Current Godot tests instantiate the addon from the development
repository rather than from a clean copied-addon project.

This is not a topology or M5 failure. It is the explicit integration work
between the completed components and the production finish line.

## Workflow audit

| Capability | Proven component state | Production integration state |
| --- | --- | --- |
| install addon | debug/release binaries load on Godot 4.6.3 and 4.7 | clean copied-addon project not yet proven |
| configure runtime | M5 limits and defaults are locked | explicit schema-1 `WorldTransvoxelConfig` now implemented and attached |
| bake world | deterministic CLI and editor path complete | no example project invokes the result as a terrain world |
| load world | manifest/page readers and corruption checks complete | asynchronous facade manifest lifecycle complete; page streaming is PQ1 |
| stream chunks | scheduler, multi-viewer union, caches, and page-meshing runtime complete | no integrated runtime coordinator behind the facade |
| render/collide | real Godot sinks and budgets complete | driven only by tests, not streamed world state |
| edit world | transaction, spatial invalidation, journal, replay, and replacement complete | no public edit capability or lifecycle ownership |
| query world | native component inspection exists | public query capability absent |
| save/reload | journal and compaction round trips complete | no facade save/reload workflow |
| migrate | native/Python migration tooling complete | not exercised by a clean application workflow |
| telemetry | bounded binary trace and 60-second orchestration soak complete | not exposed through a public runtime capability |
| shutdown | queued Godot application teardown is tested | manifest/storage lifecycle shutdown complete; meshing/edit ownership follows in PQ1/PQ2 |

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

Status: next.

- connect viewer events to desired-set, scheduler, page I/O, official MIT
  meshing, resource caches, and Godot sinks;
- provide the thin root `world_transvoxel/` example;
- prove teleport, underground, vertical, and multi-viewer movement in a baked
  world.

Exit: the example streams, renders, and collides against a real baked world
with bounded queues and correct shutdown.

### PQ2 - Editing, query, and persistence

- expose focused edit and query capabilities;
- connect edits to journal append, invalidation, replacement, and readiness;
- save, reload, compact, and migrate through the application lifecycle.

Exit: a real application edit survives save/reload and returns identical
authoritative query results.

### PQ3 - Clean install and full-world soak

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
