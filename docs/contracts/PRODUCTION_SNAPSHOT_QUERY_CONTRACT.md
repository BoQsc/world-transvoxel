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

The current in-memory compactor accepts at most 4,096 pages and 256 MiB of
source page bytes per request. Larger worlds fail explicitly and require the
streaming-compaction work tracked by PQ3 rather than risking unbounded memory.

## Proof

`test_wt_production_snapshot_query` locks this debug/release hash:

```text
PRODUCTION_SNAPSHOT_QUERY_HASH 9a4ce34af59755151263cf902f8ef9d47704ac338fb7634d282475195dad02dc
```

It proves base and edited sample values, overlap agreement and disagreement,
alignment and world-bound rejection, compaction, existing-output rejection,
compacted reopen, migration, and migrated equivalence.

`production_snapshot_query_test.gd` passes Godot 4.6.3 and 4.7 with debug and
release addon builds. It proves public edit/query ordering, immutable typed
results, nonempty-journal migration rejection, public compaction and reopen,
current-schema migration and reopen, and real schema-1.0 lifecycle migration.

PQ2 exits with an application edit surviving reload/compaction and returning
the same authoritative scalar/material value. PQ3 now additionally proves
that compaction and migration survive the isolated full-world soak matrix.
