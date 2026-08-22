# M5 Page-Meshing Runtime Contract

Status: complete and normative for the native M5 streaming baseline

## Scope

`WtPageMeshingRuntimeService` connects scheduler sample/mesh jobs to the real
asynchronous object store, authoritative page cache, decoded-page sample
source, and official MIT mesher. It owns only page-backed meshing work. Desired
sets, scheduler records, application readiness, and persistent page bytes keep
their existing owners.

The service is constructed with a fixed record capacity and an optional fixed
meshing-worker count. The hard production limits are 65,536 records, eight
meshing workers, and 25 page dependencies per record: one primary page plus
four LOD-minus-one support pages for each of six transition faces. Shared
support keys are canonicalized and requested once. Zero meshing workers is the
normative default and preserves serialized control-thread meshing.

## Job and dependency lifecycle

A sample job is accepted only when its key, generation, source revision, world
revision, stage, and scheduler lifecycle identify the scheduler's current
sampling record. The transition mask must use only six face bits, and LOD0
chunks cannot request transition support.

For every accepted job the service:

1. derives and sorts the primary/support page keys;
2. pins decoded cache hits immediately;
3. submits misses to `WtAsyncStorageService` with the job generation and
   priority, coalescing immutable page work by page key;
4. validates each completion against cache identity and fans it to all active
   records waiting for that page key;
5. submits one sample result to `WtStreamScheduler` when every page is pinned;
6. constructs `WtChunkPageSampleSource` from the primary and support pages;
7. prepares immutable page snapshots and runs `WtChunkMesher` with the official
   MIT backend, either synchronously or on the configured bounded workers;
8. releases every decoded page pin immediately after meshing succeeds or
   fails;
9. submits the mesh result to the scheduler and publishes immutable mesh
   ownership once through `pop_mesh_completion`.

The phases are `Loading`, pending sample success/failure, `AwaitingMesh`,
`Meshing`, pending mesh success/failure, and `Ready`. `Meshing` is used only
while an opt-in worker owns a prepared immutable job. A bounded record retains
canonical dependency keys after meshing so later support-page invalidation can
still find the dependent coarse generation. It retains no decoded-page pins
after mesh execution.

## Backpressure and ownership

The service has no hidden unbounded output queue. The worker input and
completion queues are each bounded by the page-meshing record capacity. If the
scheduler completion queue is full, the result remains in its bounded record
and `flush_scheduler_results` retries it after the owner drains scheduler
completions. Mesh output likewise stays in the bounded record until consumed.

Decoded pages are immutable `shared_ptr` handles. These handles remain valid
when the LRU cache evicts the same page and are released on mesh completion,
sample/mesh failure, cancellation, desired-set removal, edit replacement, or
dependency invalidation.

The storage worker does not expose per-request cancellation. Cancelling a
runtime generation removes its record and pins immediately. An already-started
successful immutable completion may still enter the cache and satisfy another
active generation waiting for the same page. It cannot resurrect or publish
the cancelled generation.

## Movement, edits, and invalidation

`WtPageMeshingRuntimeOwner` is the narrow coordination boundary used by the
desired-set and edit-replacement services:

- desired-set removal releases the page-meshing record;
- desired-set priority changes update a matching active generation;
- edit replacement cancels the superseded generation before scheduling its
  replacement.

Callers must explicitly pass the owner pointer. `nullptr` is valid only for a
runtime that has no page-meshing records; there is no default argument that can
silently omit this coordination.

`invalidate_dependency` removes every record whose canonical dependency set
contains the changed key and returns exact `(chunk key, generation)` pairs. The
runtime owner must cancel or replace those scheduler generations. This keeps
page ownership independent from scheduler policy while making invalidation
explicit and event-driven.

## Failure and concurrency policy

Invalid jobs, masks, capacities, missing manifest entries, load failures,
stale completions, cache failures, scheduler backpressure, and meshing failures
have distinct status values. Missing support fails the sample generation.
Revision disagreement or conflicting overlap in decoded support fails source
construction/meshing and never publishes partial geometry. A worker completion
must still match the page record and scheduler key, generation, source
revision, world revision, phase, and lifecycle before callbacks or publication.
Cancellation removes queued work immediately; already-running work may finish
but is discarded as stale. Priority changes update queued work but never mutate
an active immutable job. Metrics expose requests, cache hits/misses,
accepted/stale completions, failures, backpressure, cancellations,
invalidations, discarded output, preparation/completion time, worker queue
residency, execution time, active workers, and maximum simultaneous pins.

Preparation, edit replay, scheduler mutation, completion acceptance, and
publication remain single-owner and serialized. Only mesh extraction from an
immutable prepared snapshot may cross to a bounded meshing worker. Filesystem
work remains on the asynchronous storage worker. Scheduler and cache APIs are
never called by a meshing worker and retain their existing thread-safety
contracts. Causal trace mesh start/finish events identify the `meshing` role
when workers are enabled.

The asynchronous waiting queue admits at most one prepared mesh per configured
worker. When that waiting capacity is full, the next mesh remains in the
authoritative scheduler, where cancellation and interactive reprioritization
still apply. A sample job at the front of the scheduler is not blocked by mesh
admission pressure. This bound is separate from the immutable record capacity.

## Locked evidence

`test_wt_m5_page_meshing_runtime` uses a real schema-1 manifest and thirteen
content-addressed objects for one LOD1 three-face transition chunk. In debug
and release it proves:

- real asynchronous primary/support loading;
- thirteen simultaneous page pins with a two-entry decoded LRU cache;
- scheduler completion backpressure and bounded retry;
- official MIT page-backed transition meshing and post-mesh pin release;
- invalidation through a support key after meshing;
- generation cancellation and rejection of thirteen late completions;
- exact terrain-and-water mesh-hash equality between serial execution and two
  meshing workers;
- exactly paired worker start/finish events and `meshing` causal-trace roles;
- rejection of an active worker completion cancelled before publication;
- typed failure when one required support key is absent from the manifest;
- the captured human boundary edit repro keeps same-LOD chunk seams matched
  after multiple small sphere carves;
- the captured streaming-pixel transition repro keeps required fine-neighbor
  transition coverage and winding valid at a protected LOD seam corner. Extra
  protected-corner contour edges are tracked as evidence; they are not treated
  as a native missing-fine-edge failure by this contract. Godot-side visibility
  of this case is owned by the integration terrain material/visual gates;
- procedural lakes retain their locked top footprint and complete lateral
  closure across LOD0 through LOD3; the untouched reference terrain occludes
  that closure before it can appear as an open-air water wall. Generated and
  authored static water pass through render-payload construction as complete,
  unmodified Transvoxel geometry.

The workload and edit-replacement tests additionally prove desired-set
release/repriority and edit-replacement cancellation calls through the narrow
owner interface.

The locked aggregate hash is:

```text
7137658efbcc48bdb81bfa52d4e23e18acfd39d106595dfd5a136f33a01ee80b
```

This completes native support-page scheduling, pinning, cancellation, and
invalidation. Godot application budgets and production collision readiness are
also complete. The binary telemetry and fixed-duration soak evidence now pass,
so this contract participates in the completed M5 gate.
