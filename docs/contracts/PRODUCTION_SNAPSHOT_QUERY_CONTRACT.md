# Production Authoritative Query and Snapshot Contract

Status: implemented; final PQ2 unit

## Authoritative sample query

`WorldTransvoxelTerrain.request_authoritative_sample(grid_point, lod)` queues
one exact grid-sample request and returns a positive request identity. Results
are asynchronous:

- `authoritative_sample_ready(request_id, sample)`;
- `authoritative_sample_failed(request_id, error)`.

`WorldTransvoxelSample` is immutable and contains:

- requested `Vector3i` grid point and LOD;
- exact float density and uint16 material;
- source and world revisions used for reconstruction;
- number of overlapping indexed pages that agreed.

This is not interpolation. Every coordinate must align to the selected LOD's
cell spacing. The runtime locates all indexed pages whose one-cell padded
sample footprint contains the point, validates and decodes each page, replays
the complete journal through the operation's ordered world revision, and
requires exact density/material agreement. Invalid alignment, absence,
storage/page failure, replay failure, or overlap disagreement produces an
explicit rejection.

## Ordered world operations

Edits, authoritative queries, compactions, and migrations share one bounded
16-entry native operation queue. Submission order is authoritative. Therefore:

- edit then query observes the committed edit;
- query then edit observes the earlier revision;
- edit then compaction includes that edit;
- migration after an edit rejects because migration requires an empty journal.

The operation owner runs off the Godot scene thread. Disk reads, replay, and
snapshot writing do not block `_process`.

## Side-by-side snapshots

`request_world_compaction(output_directory, new_source_revision)` requires:

- a nonempty valid journal;
- a new source revision greater than the active source revision;
- a nonexistent output directory with an existing parent.

It validates every manifest page, replays the journal through the current
world revision, writes new content-addressed pages and a current-schema
manifest, and publishes the complete directory with one rename. The output
starts with no journal because its samples already contain the compacted edit
state.

`request_world_migration(output_directory)` requires an empty journal. It
validates and copies every indexed page and rewrites the manifest using the
current schema while preserving source/world revisions.

Both operations publish:

- `world_snapshot_ready(request_id, manifest_path, source_revision,
  world_revision, page_count)`;
- `world_snapshot_failed(request_id, error)`.

Snapshots are deliberately side-by-side. The live manifest and journal are
never replaced as a two-file transaction. Callers may stop the current world
and start the returned manifest when they choose. Existing output directories
are never overwritten.

Explicit manifest-backed worlds retain the in-memory limit of 4,096 source
pages and 256 MiB of source page bytes per request.

Procedural worlds use a different, bounded representation. Their complete LOD0
through LOD3 hierarchy remains an implicit `WtProceduralWorldDescriptor`; the
runtime, planner, compactor, and reopen path do not materialize the full key
catalog. A compacted procedural snapshot contains:

- checksummed `world.wtproc` geometry and revision metadata;
- a checksummed sparse `world.wtworld` overlay manifest;
- only content-addressed pages touched by durable edits and their bounded
  multiresolution support envelope.

The procedural overlay is capped at 65,536 pages and 256 MiB. The retained
128 x 16 x 128-chunk reference declares 299,520 hierarchy pages while a local
central plus finite-boundary edit compacts to 52 overlay pages. Compaction
unions prior overlays with newly affected pages, replays only the current
journal over prior overlay state, rebuilds required surface-shift records, and
publishes the descriptor last inside the unpublished staging directory.

`start_procedural_snapshot(snapshot_directory, journal_root)` reopens this
format with an independent empty journal root. Overlay pages take precedence;
all other declared pages are generated from the checksummed procedural
descriptor. Descriptor, manifest, revision, configuration-hash, key-envelope,
object-hash, byte-capacity, and page-capacity disagreement fails closed.

This sparse procedural snapshot is a current internal schema. It does not
declare universal production-save compatibility, an immutable public file
format, or support for unbounded worlds.

## Sparse hierarchy queries and accounting

`WtPageHierarchy` is the common planner/storage query boundary. It supports
exact membership, ancestor, complete-child, same-LOD face-neighbor, bounded
range, viewer-root, and LOD enumeration queries over either an explicit sorted
catalog or an implicit procedural descriptor. Missing ancestry or children
returns failure; duplicate explicit keys invalidate the hierarchy.

`get_runtime_metrics()` separates:

- declared hierarchy pages, hierarchy kind, explicit index entries, and
  estimated hierarchy-index bytes;
- sparse overlay entries and overlay-index bytes;
- encoded and decoded cache entries;
- active application records and render/collision resources;
- membership, child, ancestor, neighbor, range, viewer-root, and LOD
  enumeration query counts.

The retained implicit hierarchy reports 299,520 declared pages, zero explicit
entries, and a 40-byte descriptor index on Windows x86-64.

## Proof

`test_wt_production_snapshot_query` locks this debug/release hash:

```text
PRODUCTION_SNAPSHOT_QUERY_HASH 85405130738a0b3c38a69d1ed7f146379c7e3e470c090ff40690aec66aede7c7
```

It proves base and edited sample values, overlap agreement and disagreement,
alignment and world-bound rejection, explicit-world compaction, compacted
reopen, migration, and migrated equivalence. It also proves the implicit versus
flat hierarchy query contract, central and finite-boundary procedural edits,
two sparse compactions, reopen, migration, authoritative sample retention,
missing ancestry/children, duplicate keys, descriptor corruption, missing
manifest/object, interrupted publication, and sparse-overlay capacity
exhaustion.

`python scripts/benchmark_sparse_hierarchy.py` runs the same native authority
test repeatedly and emits p50/p95/p99/worst distributions for startup, lookup,
hierarchy traversal, edits, invalidation queries, compaction, reopen,
migration, process duration, and peak resident memory. It records the native
contract hash and executable SHA-256 with every retained report.

`production_snapshot_query_test.gd` passes Godot 4.6.3 and 4.7 with debug and
release addon builds. It proves public edit/query ordering, immutable typed
results, nonempty-journal migration rejection, public compaction and reopen,
current-schema migration and reopen, and real schema-1.0 lifecycle migration.

PQ2 exits with an application edit surviving reload/compaction and returning
the same authoritative scalar/material value. PQ3 now additionally proves
that compaction and migration survive the isolated full-world soak matrix.
