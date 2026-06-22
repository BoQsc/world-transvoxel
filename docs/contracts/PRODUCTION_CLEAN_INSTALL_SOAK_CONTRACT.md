# Production Clean-Install Full-World Soak Contract

Status: complete and normative for PQ3

PQ3 proves that the addon works after installation into a project that does
not contain the repository's application integration, build scripts, tools,
documents, references, or development artifacts.

## Clean project boundary

`scripts/test_pq3.py` constructs a new project for each engine/build case. The
project contains:

- the copied `addons/world_transvoxel/` distribution;
- root 0BSD and MIT scope/notices;
- a minimal generated `project.godot`;
- the PQ3 test driver;
- the deterministic 28-page baked-world fixture.

The harness rejects links, unexpected top-level content, missing notices, or
notice text that differs from the repository authority. It performs a fresh
headless Godot import and verifies that the generated extension cache names
only the copied addon GDExtension before running the workload.

The checked distributions contain 143 files. Their current identities are:

| Runtime image | Bytes | Distribution SHA-256 |
| --- | ---: | --- |
| Windows x86-64 template debug | 3,922,766 | `0ec14d15979972d8ab60ea119074d29653a74be78bd24e2c5fa2bdbcc54a66f3` |
| Windows x86-64 template release | 3,874,126 | `7c78903886d65ce89b2421193142cb21e7a0bd8be6b7df9d269bb055e73d7966` |

These are clean-install test-image identities, not the final reproducible
release artifact. PQ4 owns the release artifact definition.

## Full-world workflow

Each case runs for at least 15,000 monotonic milliseconds and then completes
the persistence/reopen sequence. The test:

- opens the real 28-page, two-LOD baked world;
- queries one authoritative sample from every indexed page;
- moves one and two viewers across the complete X extent;
- repeatedly exercises the 5/6/9/10/13-chunk balanced LOD states;
- observes regular and transition render/collision resources;
- commits material edits against a freshly baked revision-zero world;
- submits ordered authoritative queries after those edits;
- compacts all pages into a new source revision;
- stops and reopens the compacted world;
- migrates the compacted world;
- stops and reopens the migrated world;
- verifies authoritative value equivalence and clean final shutdown.

Revision zero is a valid initial world revision. The public edit API must allow
the first transaction to use base revision zero and commit revision one.

## Runtime metrics snapshot

`WorldTransvoxelTerrain.get_runtime_metrics()` returns an immutable
`Dictionary` copied from native runtime/application counters. It includes
viewer, page, meshing, transition, edit, query, snapshot, publication,
application, latency, active-resource, and queue counters. It does not expose
native containers or mutable ownership.

The soak requires zero runtime event rejections, edit/query/snapshot
rejections, application queue rejections, and sink failures. Cancellation and
stale-application counters may be nonzero because replacement and movement
intentionally invalidate older generations.

## Locked limits

Every matrix case must satisfy:

- static-memory peak no greater than 768 MiB;
- maximum sampled frame interval no greater than 2,000,000 microseconds;
- p99 sampled frame interval no greater than 250,000 microseconds;
- at most 40 render resources, collision resources, and queued applications;
- at most 32 frames maximum render/collision application latency;
- at least 10 distinct observed render and collision chunk identities;
- at least four transition identity/readiness checks;
- all 28 baked pages queried;
- at least one durable edit and one transition mesh completion;
- zero retained resources and application queues after shutdown.

The checked Windows x86-64 evidence across Godot 4.6.3/4.7 and
template-debug/template-release recorded:

- 24,767,031 to 28,008,695 bytes static-memory peak;
- 15,560 to 39,905 microseconds p99 frame interval;
- 61,529 to 107,583 microseconds maximum frame interval;
- 13 maximum render and collision resources;
- zero maximum render/collision application queue depth;
- one frame maximum render and collision application latency;
- 135 to 156 movement steps and 270 to 312 transition checks per case;
- 12 committed edits, 42 authoritative queries, one compaction, one migration,
  and clean shutdown per case.

Geometric seam correctness remains a composed gate: the clean soak proves the
official transition path remains active under real page I/O and replacement,
while the exhaustive M2 and decoded-page M5 seam suites prove geometric
agreement.

## Matrix and evidence

Required cases:

- Godot 4.6.3 with template-debug addon;
- Godot 4.7 with template-debug addon;
- Godot 4.6.3 with template-release addon;
- Godot 4.7 with template-release addon.

Reference evidence:

```text
docs/evidence/pq3_clean_install_soak_windows_x86_64.json
```

Routine command:

```console
python scripts/test_pq3.py
```

Reference refresh:

```console
python scripts/test_pq3.py --write-reference-evidence
```

PQ3 exits only when every matrix case passes this contract. PQ4 release
qualification is the remaining production gate.
