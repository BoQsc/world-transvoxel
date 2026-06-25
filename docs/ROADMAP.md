# Finite Roadmap

Each milestone has an exit condition. Passing a milestone does not
automatically create another one.

## R0 - Research lock

Status: complete.

- references downloaded and hashed;
- upstream revisions pinned;
- existing Marching Cubes postmortem recorded;
- architecture, API, binary format, and coding standards reviewed.

Exit: no terrain implementation exists before these documents pass review.

## M0 - Addon and upstream baseline

Status: complete on 2026-06-20 for Windows x86-64, Godot 4.6.3 and 4.7.

- self-contained `addons/world_transvoxel`;
- official MIT files unchanged and isolated;
- root 0BSD/MIT scope validation;
- pinned godot-cpp and Zig stable toolchain;
- debug and release builds;
- upstream provenance test.

Exit: addon loads on the primary Godot baseline and one later minor version.

## M1 - Exact native cell backend

Status: complete on 2026-06-20 for Windows x86-64.

- typed native API;
- exhaustive 256 regular and 512 transition cases;
- six transition orientations;
- deterministic hashes;
- no scene or streaming code.

Exit: exact MIT backend contract passes headless tests.

## M2 - Chunk and LOD ownership

Status: complete on 2026-06-20 for Windows x86-64.

- native chunk records and generation tokens;
- 2:1 LOD invariant;
- transition ownership rules;
- neighbor seam and corner tests;
- event-driven job graph.

Exit: deterministic multi-chunk galleries have zero open LOD seams.

## M3 - Render and collision integration

Status: complete on 2026-06-20 for Windows x86-64, Godot 4.6.3 and 4.7.

- main-thread application queues;
- native mesh resources;
- collision generation and distance policy;
- cancellation and stale-result rejection;
- bounded frame application.

Exit: moving-viewer integration test passes without unbounded queues or frame
thread readback.

## M4 - Storage, baking, and editing

Status: complete on 2026-06-21 for Windows x86-64, Godot 4.6.3 and 4.7.

Versioned bounded containers, SHA-256 identities,
standalone range-addressable chunk pages, deterministic native page baking,
content-addressed world manifests/indexes, atomic typed edit transactions, and
bounded padded-sample spatial invalidation, append-only journal recovery, and
deterministic authoritative sample replay and audited snapshot compaction
complete. Native-backed inspect, validation, and world migration tools are
also complete, together with the deterministic dense-volume command-line
baker and its thin Godot editor entry point. The dense baker now streams
validation, samples through a fixed file cache, and writes one page at a time;
page output remains byte-identical to the 1.0.2 baker while the world manifest
hash remains release-versioned through generator provenance. The schema-1 codec
decision is resolved as `none` only.

- versioned binary world/chunk/edit formats;
- deterministic baker;
- range-loadable chunk pages;
- edit transactions and spatial invalidation;
- migrations and corruption tests.

Exit: bake-load-edit-save-reload produces identical authoritative state.

## M5 - Streaming production baseline

Status: complete on 2026-06-21 for Windows x86-64, Godot 4.6.3 and 4.7.

- multiple viewers;
- asynchronous manifest/page storage with content-addressed validation
  (complete);
- authoritative encoded-page and decoded-sample caches (complete);
- derived mesh, render, and collision caches (complete);
- multi-viewer desired-set scheduling without idle world scans (complete);
- edit invalidation and generation-cancelled runtime replacement (complete);
- teleport, fast vehicle, underground, vertical-world, edit, and multi-viewer
  functional workloads (complete);
- native orchestration latency and process-memory budgets on reference
  hardware (complete);
- warm page I/O/decode, decoded LOD0 MIT meshing, and native higher-LOD MIT
  transition-meshing budgets on reference hardware (complete);
- higher-LOD transition sample ownership and integrated decoded-page seam
  evidence (complete);
- runtime support-page dependency scheduling, pinning, and invalidation
  (complete);
- Godot render/physics application budgets and production collision/readiness
  policy (complete);
- binary telemetry and fixed-duration soak evidence (complete).

Exit: fixed-duration motion/edit workload meets queue, memory, seam, collision,
and frame-time budgets.

## M6 - Optional compute acceleration

Status: optional and deferred; it is not a production-release prerequisite.

- measured CPU baseline retained;
- compute density/bake path;
- asynchronous transfer strategy;
- device-loss and unsupported-device fallback;
- no regression in determinism or headless operation.

Exit: compute is enabled only where end-to-end measurements beat CPU.

## Production finish line

Status: complete on 2026-06-25 for World Transvoxel 1.0.5.

World Transvoxel 1.0.0 is withdrawn due to incorrect Godot-facing winding and
incomplete convex mixed-LOD corner deformation.

World Transvoxel 1.0.1 is superseded because moving-viewer LOD changes retired
old chunks before replacement application. Version 1.0.2 stages retirement
until replacement render/collision readiness is confirmed. Version 1.0.3 is
superseded by 1.0.4 because dynamic mixed-LOD viewer motion could retire a
large ready replacement set in one frame; 1.0.4 keeps staged retirement and
adds a bounded per-frame retirement flush. Version 1.0.5 adds a fixed native
render fade-out window for retiring chunks after replacement application.

Finite qualification gates are tracked in
`docs/production/QUALIFICATION_AUDIT.md`:

- PQ0 configuration and lifecycle (complete);
- PQ1 read-only real baked-world streaming (complete: LOD0, balanced
  multi-LOD transitions, and root real-scene example);
- PQ2 editing, query, and persistence (complete: readiness and authoritative
  sample queries, lifecycle journal ownership, atomic edits, edited-page
  replacement, restart replay, side-by-side compaction, and migration);
- PQ3 clean install and fixed-duration full-world soak (complete);
- PQ4 release qualification (complete: deterministic directory release,
  installed tool audit, notices/provenance, shipped API/limits, and exact
  release install matrix).

- official MIT backend remains the reference;
- full-world soak test;
- save migration test;
- supported Godot/platform matrix;
- documented operational limits;
- reproducible release artifact.

The 0BSD backend may now enter the same contract and production qualification
suite. It does not replace MIT based on isolated case tests alone.
