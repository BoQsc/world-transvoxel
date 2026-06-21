# M5 Asynchronous Storage Contract

Status: implemented foundation

## Responsibility

`WtAsyncStorageService` owns an immutable `wtworld` manifest copy and resolves
its indexed pages from a content-addressed filesystem object store. Page I/O
occurs on one native worker thread. The caller submits typed chunk requests and
polls or waits for typed completions.

The service does not own desired LOD, decoded samples, meshes, Godot resources,
or current chunk generations. Those remain scheduler/cache/application
responsibilities.

## Object resolution

For an indexed page hash, the only accepted object name is:

```text
<64 lowercase SHA-256 hex digits>.wtchunk
```

The object root is fixed when the service opens. Request data cannot contribute
path separators or arbitrary filenames.

Every completed read validates, in order:

1. the object exists and its filesystem size equals the manifest entry;
2. the full bytes are readable within the configured page limit;
3. SHA-256 equals the manifest identity;
4. the bytes decode as a valid `wtchunk`;
5. chunk key and source revision match the immutable world manifest.

Invalid bytes never appear in a successful completion.

## Bounded asynchronous behavior

The constructor fixes:

- request queue capacity;
- completion queue capacity;
- maximum accepted page byte size.

Queue capacities are nonzero and at most 65,536. The page limit is between the
common container header size and 256 MiB. Opening rejects a manifest containing
any page larger than the configured limit.

The request queue is ordered by descending integer priority and then FIFO
sequence. Exact duplicate `(chunk key, generation)` requests are rejected while
pending. A full request queue rejects new work without mutating accepted work.

The worker sleeps on a condition variable when idle. It never scans the world
manifest for work. A full completion queue applies backpressure to the worker;
completed results are not dropped to admit newer results.

## Generation and ownership

Every request requires a nonzero `WtGenerationToken`. The token is copied
unchanged into its completion. The returned byte vector is immutable shared
ownership.

This foundation deliberately does not decide whether a completion is stale.
The later M5 scheduler integration must compare the completion token against
the current chunk generation before decoding, caching, meshing, or applying
resources.

## Shutdown

`close()` wakes the sleeping worker, rejects later requests, joins the worker,
and clears queued requests, completions, manifest bytes, and paths. Metrics
record queued or in-flight work discarded by shutdown.

## Evidence

`test_wt_m5_async_storage` creates a real five-page world and filesystem object
store. It proves:

- asynchronous successful loading and byte identity;
- generation-token preservation;
- priority ordering under bounded completion backpressure;
- request overflow rejection;
- duplicate request rejection;
- missing, short, hash-corrupt, and metadata-mismatched object failures;
- invalid configuration, path, manifest, key, and page-size rejection;
- no unrequested work while idle;
- drained queues and closed-service rejection;
- matching debug/release evidence hash:

```text
96ba7123c6b86fe9e2f07aa17f27553f58db92cca6427a069eced98de1471402
```

This contract completed the first M5 work item. Bounded caches, multi-viewer
runtime ownership, edit-driven replacement, and representative functional
workloads are now also complete. Hardware budgets, binary traces, and soak
evidence remain active M5 work.
