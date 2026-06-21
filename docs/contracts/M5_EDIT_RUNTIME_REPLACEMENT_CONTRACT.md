# M5 Edit Runtime Replacement Contract

Status: implemented coordination baseline

## Responsibility

`WtEditRuntimeReplacementService` converts one committed edit transaction into
a bounded replacement event for the loaded chunks returned by
`WtEditSpatialIndex`. It coordinates scheduler generations, authoritative
page/sample cache ownership, derived resource cache ownership, and render and
collision readiness.

It does not scan an entire world, commit journal records, execute worker
sampling or meshing, mutate Godot resources, or choose desired LOD. Those
remain owned by the spatial index, edit journal, worker pipeline, application
sinks, and desired-set service respectively.

## Version identity

Runtime chunk and job identity now carries two independent revisions:

```text
source_revision  immutable baked snapshot or deterministic source identity
world_revision   latest ordered edit revision required by this generation
```

An edit preserves `source_revision` and advances affected jobs to the
transaction's `committed_revision`. Treating an edit as a new source revision
would break journal replay and snapshot-compaction identity.

## Event and bounded lookup

The edit commit event supplies a validated `WtEditTransaction`. The service
queries the existing padded-sample spatial index, which returns only loaded
same-LOD and dependent coarser-LOD keys. The spatial index is maintained from
load/unload events; replacement does not perform an idle or full-world scan.

Construction fixes a nonzero replacement capacity no greater than 65,536.
Transactions whose affected loaded set exceeds that capacity fail before any
runtime state or cache mutation.

## Preflight and batch rejection

Before replacing any affected chunk, the service verifies all of the following:

- a non-cancelled scheduler record exists;
- a matching application-readiness record exists;
- scheduler and application generations are identical;
- the loaded source revision matches the transaction source revision;
- the chunk is not ahead of the transaction base revision;
- the scheduler job queue has capacity for every affected replacement.

Invalid spatial input, capacity exhaustion, source/world revision mismatch,
and missing or divergent runtime state reject the complete batch. These
expected rejection paths do not advance any generation, reset readiness,
evict cache entries, or enqueue a partial replacement prefix.

The scheduler and all coordinated cache/application mutations are single-owner
operations. Worker threads may consume jobs and submit immutable completions,
but they do not issue competing replacement requests.

## Successful replacement

For each canonical affected key, a successful replacement:

1. requests a new sample job with the original source revision, committed
   world revision, existing priority, and a new generation token;
2. cancels matching page-meshing ownership before replacement when that owner
   exists;
3. makes queued or in-flight storage, sample, mesh, render, and collision work
   from the previous generation stale;
4. resets visual and collision readiness to false while preserving whether
   collision is currently required;
5. releases encoded and decoded page-cache ownership for the key;
6. releases mesh, render, and collision cache ownership for the key;
7. records the old/new generations and exact eviction counts.

The normal scheduler lifecycle then executes `Sample -> Mesh -> Ready` for the
new world revision. Generation checks prevent old completions from entering
caches or reaching render/physics sinks. New render and required collision
payloads independently restore application readiness.

External immutable cache handles can outlive eviction, as defined by the cache
contracts, but cannot publish after their generation is superseded.

## Metrics

Metrics expose transaction attempts/completions, empty loaded intersections,
queried and replaced chunks, page/resource entries released, spatial and
capacity rejections, state/revision rejections, and invariant-level scheduler
or application failures. They also report cancelled page-meshing generations
and page-meshing coordination failures.

## Evidence

`test_wt_m5_edit_replacement` proves in debug and release builds:

- one edit invalidates both a same-LOD chunk and its loaded coarser dependent,
  while leaving an unrelated chunk resident;
- source and world revisions remain distinct through sample and mesh jobs;
- page/sample and mesh/render/collision ownership is released and rebuilt;
- every affected page-meshing generation receives an exact cancellation call;
- old storage, scheduler, render, and collision completions are stale;
- visual and collision readiness reset and recover independently;
- insufficient queue capacity and missing application state reject atomically;
- 1,000 repeated edit replacements retain one scheduler record, one
  application record, and bounded empty queues.

The locked deterministic evidence hash is:

```text
03eedad6263963350d32226bd5c59f9aba48e4b35adefa4f2cd774cd70cfb9df
```

This completes the edit-driven runtime replacement coordination unit, including
the page-meshing owner boundary. Representative functional workload, native
component budgets, Godot application budgets, and production collision
readiness are also complete. Binary telemetry and fixed-duration soak evidence
now pass as the final M5 unit.
