# M4 Snapshot Compaction Contract

Status: normative M4 component

Snapshot compaction reconstructs authoritative samples from a verified baked
world plus its ordered edit journal, writes new standalone chunk pages, and
creates a new `wtworld` manifest. It does not copy edited cache pages supplied
by a caller; it replays the journal itself over every verified source page.

## Inputs and validation

Compaction requires:

- one valid previous `wtworld` manifest;
- exactly every page indexed by that manifest;
- complete page bytes whose size, full SHA-256, key, and source revision
  match the manifest;
- a nonempty journal whose source revision equals the world source revision;
- a journal initial world revision equal to the world manifest revision;
- a new source revision greater than the previous source revision;
- an explicit page capacity.

Page input order is irrelevant. Keys are sorted canonically and must match the
world index exactly.

## Reconstruction

For each source page:

1. validate it through the world index;
2. decode its authoritative 19-cubed samples;
3. initialize edit state at the world manifest revision;
4. replay the complete ordered journal;
5. require the page state to reach the journal's current revision;
6. change only the page source revision to the new snapshot revision;
7. serialize and hash the new standalone page.

The new manifest preserves the configuration and ordinary dependency
identities, indexes the new page hashes and sizes, and stores:

```text
source_revision = new snapshot source revision
world_revision  = compacted journal revision
```

The edit history is now represented by the new authoritative page samples.
A subsequent empty journal starts from the new source revision and persisted
world revision, so its next transaction continues monotonically.

## Audit chain

Two reserved source-asset dependency labels retain the compaction audit:

```text
compaction/previous-world  SHA-256 of complete previous wtworld bytes
compaction/edit-journal    SHA-256 of complete concatenated wtedit bytes
```

On repeated compaction, old entries with these labels are replaced. The new
previous-world hash still commits to the prior manifest and therefore to the
entire older audit chain without unbounded dependency growth.

The returned `WtCompactionAudit` also records previous/new source revisions,
initial/compacted world revisions, and the complete new-world hash.

## World schema 1.1

Compaction requires persistent world edit revision state. `META` schema 1.1
therefore extends schema 1.0 from 64 to 72 bytes:

| Offset | Type | Meaning |
| ---: | --- | --- |
| 64 | `u64` | current authoritative world edit revision |

Readers continue to accept schema 1.0's exact 64-byte metadata and assign
world revision zero. Writers always emit schema 1.1. Other metadata,
dependency, and index fields retain their existing meanings.

## Atomic output

Compaction builds pages, manifest, and audit in replacement objects. Any
world, journal, page, replay, serialization, or capacity failure clears output
and leaves caller-owned inputs unchanged. Publishing the resulting files and
switching the active snapshot remain a storage-service atomic replace step.

## Evidence

`tests/native/test_wt_m4_compaction.cpp` locks the debug/release compacted
snapshot hash:

```text
e49dacbc1d728125f4846e7ca4dd3b19d7374b8df96fb346a23aca74d553293f
```

The test covers:

- page-input-order-independent compaction;
- new page and world hash agreement;
- direct replay versus compacted sample equality;
- world revision persistence and next-journal continuation;
- previous-world and journal audit identities;
- missing, corrupt, and over-capacity page sets;
- source/journal disagreement;
- empty journal and non-increasing source revision rejection.

`tests/native/test_wt_m4_world.cpp` also constructs a schema-1.0 `META`,
`DEPS`, and `INDX` fixture and proves it loads with world revision zero.

This component satisfies the native bake-load-edit-save-reload state loop.
M4 still requires user-facing bake/inspect/validation tools, broader migration
and corruption fixtures, exact milestone evidence, and the controlled codec
decision before closure.
