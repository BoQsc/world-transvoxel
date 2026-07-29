# M5 Asynchronous Storage Contract

Status: implemented foundation

## Responsibility

`WtAsyncStorageService` owns an immutable `wtworld` manifest copy and resolves
its indexed pages from a content-addressed filesystem object store. File-backed
page I/O occurs on one native worker thread. Procedural immutable page
generation uses a separately configured bounded pool of one to eight workers;
the production default is two. The caller submits typed chunk requests and
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
sequence. Because a source page is immutable for the open world revision,
duplicate chunk-key requests coalesce even when consumer generations differ.
A duplicate may raise the queued request's priority but cannot start redundant
storage or procedural work. A full request queue rejects new work without
mutating accepted work.

The worker sleeps on a condition variable when idle. It never scans the world
manifest for work. A full completion queue applies backpressure to the worker;
completed results are not dropped to admit newer results.

## Generation and ownership

Every request requires a nonzero `WtGenerationToken`. The token is copied
unchanged into its completion. The returned byte vector is immutable shared
ownership.

The completion retains the generation of the request that owned the shared
load. The page-meshing layer fans one successful immutable completion to every
currently waiting generation for the same page key. A cancelled first owner
does not invalidate the page bytes: a successful completion may still enter
the page cache for coalesced or future consumers. Scheduler/application
generation checks remain mandatory before meshing or publishing resources.

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
- duplicate request coalescing and priority update;
- byte-identical procedural output with four generation workers;
- missing, short, hash-corrupt, and metadata-mismatched object failures;
- invalid configuration, path, manifest, key, and page-size rejection;
- no unrequested work while idle;
- drained queues and closed-service rejection;
- matching debug/release evidence hash:

```text
9aca3433346bf02392062fea7ffd5ee1edddda81b1245c844e642517f4b1517c
```

This contract completed the first M5 work item. Bounded caches, multi-viewer
runtime ownership, edit-driven replacement, and representative functional
workloads, native component budgets, real page-meshing dependency jobs, Godot
application budgets, and production collision readiness are now also complete.
Binary traces and the fixed-duration soak now pass, completing M5.
