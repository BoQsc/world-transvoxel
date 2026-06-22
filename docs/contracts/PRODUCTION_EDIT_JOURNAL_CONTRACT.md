# Production Edit Journal Contract

Status: implemented; second PQ2 unit

## Public capability

`WorldTransvoxelTerrain.begin_edit_transaction(author_id)` returns a typed
`WorldTransvoxelEditTransaction` at the current source and world revisions.
The transaction accepts sphere and axis-aligned-box commands for:

- additive density;
- absolute density;
- material paint.

`commit_edit_transaction(transaction)` validates and queues one non-empty
transaction. Queue acceptance is synchronous. Final success or rejection is
asynchronous through:

- `edit_committed(world_revision)`;
- `edit_failed(error)`.

A submitted object cannot be edited or submitted again. Transactions created
at the same base revision may both enter the queue, but only the first valid
commit advances the world; later stale transactions are rejected without
changing durable bytes.

## Lifecycle ownership and ordering

Each world object root owns `world.wtedit`. The object root is therefore one
authoritative editable-world namespace. Startup opens the baked manifest,
then loads this journal against the manifest source and initial world
revisions.

A missing or empty journal starts at the manifest revision. A valid incomplete
final segment is recovered by retaining and durably truncating to the complete
prefix. Interior corruption, source mismatch, non-monotonic revisions,
duplicate identities, and capacity overflow fail or reject explicitly.

Commit ordering on the native runtime owner thread is:

1. validate source/base/committed revisions;
2. preflight the affected loaded-chunk replacement;
3. append, flush, and synchronize the canonical `WTEDIT` segment;
4. advance the authoritative world revision;
5. cancel superseded page-meshing ownership, evict affected caches, and
   request replacement generations;
6. publish replacement expectations and the commit event.

No commit success is published before durable append. A failure after durable
append is a fatal runtime integration error because rollback would make
persisted and rendered state disagree.

## Edited-page replay

Every mesh generation carries its required world revision. Before official MIT
meshing, each decoded primary/support page is copied into
`WtChunkEditState`, journal commands are replayed through that exact revision,
and the immutable edited copies become the sample source.

This applies equally to newly streamed chunks, loaded-generation replacement,
and restart. Replay failure is fatal to the production runtime rather than
silently publishing pre-edit terrain.

## Fixed limits

The production journal is bounded to:

- 4,096 transactions;
- 65,536 commands;
- 64 MiB of canonical transaction bytes.

Compaction is required before those limits are exceeded. Public lifecycle
compaction and authoritative scalar/material queries remain the next PQ2 unit.

## Proof

`test_wt_production_edit_journal` locks this debug/release hash:

```text
PRODUCTION_EDIT_JOURNAL_HASH f6331a4a71a1c1ddb4bfd2aca61562dd5358710a9888eabd6b1b9308229e64e6
```

It proves two durable commits, restart loading, physical truncated-tail
recovery, interior-corruption and source-mismatch rejection, and byte-stable
duplicate/stale rejection.

`production_edit_journal_test.gd` passes Godot 4.6.3 and 4.7 with debug and
release addon builds. It proves typed public construction, invalid-parameter
rejection, an affected generation changing from a real surface to a ready
empty surface, stale concurrent rejection, journal creation, clean stop/start,
and identical empty terrain after restart replay.
