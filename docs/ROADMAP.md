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

Status: in progress; versioned bounded containers, SHA-256 identities,
standalone range-addressable chunk pages, deterministic native page baking,
content-addressed world manifests/indexes, atomic typed edit transactions, and
bounded padded-sample spatial invalidation, append-only journal recovery, and
deterministic authoritative sample replay and audited snapshot compaction
complete. Native-backed inspect, validation, and world migration tools are
also complete, together with the deterministic dense-volume command-line
baker and its thin Godot editor entry point. The schema-1 codec decision is
resolved as `none` only.

- versioned binary world/chunk/edit formats;
- deterministic baker;
- range-loadable chunk pages;
- edit transactions and spatial invalidation;
- migrations and corruption tests.

Exit: bake-load-edit-save-reload produces identical authoritative state.

## M5 - Streaming production baseline

- multiple viewers;
- bounded caches and eviction;
- teleport, fast vehicle, underground, and vertical-world tests;
- rendering and collision readiness APIs;
- representative telemetry.

Exit: fixed-duration motion/edit workload meets queue, memory, seam, collision,
and frame-time budgets.

## M6 - Optional compute acceleration

- measured CPU baseline retained;
- compute density/bake path;
- asynchronous transfer strategy;
- device-loss and unsupported-device fallback;
- no regression in determinism or headless operation.

Exit: compute is enabled only where end-to-end measurements beat CPU.

## Production finish line

- official MIT backend remains the reference;
- full-world soak test;
- save migration test;
- supported Godot/platform matrix;
- documented operational limits;
- reproducible release artifact.

After this point, the 0BSD backend may enter the same contract and production
qualification suite. It does not replace MIT based on isolated case tests
alone.
