# M4 Append-Only Edit Journal Contract

Status: normative M4 component

A `.wtedit` journal is an ordered concatenation of complete schema-1
`WTEDIT` transaction containers. Each segment retains its common-container
hashes, per-command hashes, and atomic `CMIT` footer.

This segmented representation makes append behavior explicit:

1. prepare the next canonical transaction segment without changing state;
2. append and durably flush the exact segment through the storage service;
3. commit that segment to the in-memory journal revision state.

If external persistence fails between steps 1 and 3, memory does not advance.
The convenience in-memory `append` operation performs prepare and commit
together for callers that do not own external storage.

## Ordering invariants

A journal is initialized with:

- one baked source revision;
- one initial world edit revision;
- construction-time transaction, command, and byte capacities.

Every appended transaction must:

- use the journal source revision;
- have `base_revision` equal the journal's current revision;
- commit exactly `current_revision + 1`;
- have a transaction ID not previously used by the journal;
- have command IDs not previously used by the journal;
- fit all three capacities.

Transaction parsing already proves canonical command sequences, command world
revisions, shapes, bounds, operation values, and commit hashes.

Prepared or loaded transactions are canonicalized before storage. Replay
therefore visits transactions by committed revision and commands by sequence,
independent of the caller's original vector ordering.

## Segment discovery

`wt_measure_container` reads only the bounded common header and section
directory needed to determine the first complete container length in a byte
prefix. It validates version, required features, directory ordering, codecs,
contiguous section ranges, and common size limits. Full hashes and transaction
semantics are then validated by `wt_open_edit_transaction` over exactly that
segment.

This permits deterministic scanning of concatenated transactions without
placing native sizes or filesystem metadata in the journal.

## Load and recovery

Normal load is atomic: any invalid segment leaves the destination journal
unchanged.

Optional truncated-tail recovery is narrowly defined. If the final measured
transaction is incomplete, all preceding fully verified segments become the
loaded journal and the loader reports the exact committed byte prefix.
Callers may truncate the physical file to that prefix before further append.

Recovery does not accept:

- a hash mismatch in a complete segment;
- invalid magic, version, directory, codec, or section metadata;
- a source revision mismatch;
- a world revision gap or fork;
- duplicate transaction or command IDs;
- a noncanonical transaction encoding.

Those conditions fail atomically as journal corruption or ordering errors.

## Replay

Replay sends immutable commands in exact journal order to a typed native
`WtEditReplaySink`. A sink failure stops replay immediately and is reported.
Replay does not mutate journal state or hide partial sink application; callers
that need transactional destination behavior must replay into a replacement
snapshot and swap it only after success.

## Evidence

`tests/native/test_wt_m4_journal.cpp` locks the debug/release journal hash:

```text
82ba948c7f37e5812e5fc40331cf7b07c2fbad58a63903f484448d9dcf71de36
```

The test covers:

- three contiguous transaction segments and six ordered commands;
- canonical save/load/save byte agreement;
- first-segment size discovery within concatenated bytes;
- prepare without state mutation and explicit commit;
- replay ordering and sink failure;
- strict truncated-tail failure and optional committed-prefix recovery;
- complete-segment corruption with atomic destination preservation;
- source and world revision mismatch;
- duplicate transaction and command IDs;
- transaction, command, and byte capacity limits.

Ordered commands now apply directly to authoritative chunk samples. This
component does not complete M4. Bake-load-edit-save-reload equivalence,
compaction into a new snapshot with audit identity, migrations, and
user-facing bake/inspect tools remain active work.
