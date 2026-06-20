# M2 Chunk, LOD, and Scheduling Contract

Status: normative for M2 and later runtime work

## Coordinate model

- Base-grid coordinates are signed 64-bit integers.
- A LOD `L` cell has base-grid size `2^L`.
- A chunk always contains 16 cells per axis and has extent `16 * 2^L`.
- A chunk key contains signed 32-bit coordinates and a LOD in `0..20`.
- Chunk bounds are half-open: `[minimum, maximum)`.
- Negative parent coordinates use mathematical floor division. For example,
  chunk coordinate `-1` at LOD 0 belongs to parent coordinate `-1` at LOD 1.

The LOD limit keeps all signed 32-bit chunk keys representable in signed
64-bit base-grid coordinates.

## Active LOD map

Active chunks are non-overlapping octree leaves. The map rejects:

- invalid or duplicate keys;
- more entries than its configured capacity;
- parent/child or other volume overlap;
- face-adjacent leaves whose LOD values differ by more than one.

Edge-only and corner-only contact is not a face-neighbor relationship. A
partial positive-area face overlap is a neighbor relationship and therefore
participates in the 2:1 check.

## Transition ownership

The coarser leaf owns the transition patch on every face touching finer leaves
at exactly one lower LOD. The finer leaves never emit a competing patch.

Face bits are stable internal values:

```text
bit 0  -X
bit 1  +X
bit 2  -Y
bit 3  +Y
bit 4  -Z
bit 5  +Z
```

One coarse face may touch four finer chunks; this still sets one owner bit.
Multiple bits may be active at LOD edges and corners. Meshing must consume the
validated mask and must not infer or repair an invalid global LOD map.

## Chunk lifecycle and generations

Each chunk record owns:

- a chunk key;
- a nonzero monotonically issued generation token;
- source revision;
- priority;
- lifecycle state;
- transition ownership mask.

The M2 lifecycle is `Requested/Sampling/Meshing/Ready/Cancelled/Failed`.
Requesting changed data issues a new generation. Cancellation also issues a
new generation, immediately making already queued or running results stale.

## Typed bounded jobs

Sampling and meshing are distinct `WtChunkJobStage` values. Requests and
results contain concrete C++ fields; no string keys, Godot variants, or scene
objects cross worker queues.

- The job queue is bounded and ordered by descending priority, then ascending
  issue sequence.
- The completion queue is a bounded FIFO.
- Queue overflow is an explicit status and metric.
- Worker job pops and completion submissions are synchronized.
- Records, viewer snapshots, and queues never grow beyond construction-time
  capacities.
- A sampling success enqueues meshing for the same generation.
- A meshing success makes that generation ready.
- Failed jobs enter `Failed`.
- Missing, cancelled, superseded, duplicate-stage, and otherwise mismatched
  results are counted and discarded.
- Applying no completions performs no record scan or work.

Viewer snapshots are immutable values identified by ID and monotonic revision.
Stale viewer revisions and capacity overflow are rejected.

## Current proof

`tests/native/test_wt_m2_core.cpp` covers negative coordinates, parent mapping,
same-LOD leaves, valid 2:1 ownership, two-face LOD corners, overlap rejection,
invalid LOD differences, priority order, lifecycle transitions, cancellation,
stale-result rejection, viewer revisions, queue capacity, and 1,000 repeated
supersession cycles with bounded state.

Chunk mesh and geometrical seam proofs are the remaining M2 work and must
extend this contract before M2 is marked complete.
