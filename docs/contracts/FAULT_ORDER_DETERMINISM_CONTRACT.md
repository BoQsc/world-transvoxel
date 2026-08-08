# Fault Injection And Cross-Order Determinism Contract

## Status

This contract defines the upstream authority used by Terrain Qualification
Program milestone TQP-43. It qualifies failure handling and convergence at the
stream scheduler, desired-set, asynchronous page-storage, and snapshot
publication boundaries. It does not claim that arbitrary process-wide memory
exhaustion can be recovered after the C++ allocator terminates.

## Deterministic Completion Order

Sample and mesh workers may complete in any order. Once all accepted results
have been applied, equal logical requests must produce equal authoritative
chunk records and equal published-resource signatures. Superseded generation
results and duplicate completions are stale input; they must not mutate the
current record and must increment stale-result accounting.

The qualification workload runs 64 deterministic shuffles over eight records.
It includes one cancellation and replacement, one duplicate sample result, one
duplicate mesh result, one stale pre-cancellation result, and one declared
sample failure. Every shuffle must settle to the same sorted authoritative
trace. The declared failure must remain `Failed` and must not publish mesh work.

## Rapid Viewer Motion

Viewer updates may arrive faster than downstream work completes. A desired set
must settle to the final accepted viewer revision independent of prior motion
order. An older or duplicate viewer revision is rejected as stale and cannot
replace the accepted final state.

## Explicit Fault Sites

Fault injection is disabled by default and has no effect until explicitly
armed. `wt_arm_fault_injection()` selects one site and the matching attempt on
which it fails. A successful injection disarms itself. Metrics retain matching
attempt, injected-failure, and remaining-match counts until the next arm.

The qualified sites are:

- `PageBufferAllocation`: page generation or file loading returns
  `WtPageLoadStatus::AllocationFailure` without publishing page bytes.
- `SnapshotWorkspaceAllocation`: migration or compaction returns
  `WtWorldSnapshotStoreStatus::AllocationFailure` before staging or publishing
  an output directory.

These sites are explicit admission boundaries because the extension toolchain
disables C++ exceptions. They validate deterministic fail-closed behavior at
allocating operations; they are not a promise to recover from an allocator
abort at an unguarded internal allocation.

## Storage And Shutdown Fail-Closed Rules

- A duplicate active page request is coalesced and reported as already pending.
- After an injected page failure, a later request may succeed normally.
- Shutdown joins workers and leaves no queued request, completion, or active
  request. Every accepted request is completed or cancelled.
- A pre-existing snapshot staging directory is treated as interrupted external
  state. Publication fails without replacing it or exposing a final snapshot.
- Invalid output paths and malformed procedural snapshot descriptors fail with
  a declared status and do not open as authoritative worlds.
- A valid publication after fail-closed cases must reopen successfully.

## First-Divergence Trace

`WtDeterminismTraceEvent` identifies a chunk key, generation, event kind,
authoritative-state signature, and resource signature. Traces are compared in
their canonical logical order. A mismatch reports its event index and the first
divergent generation. Missing and unexpected events are distinct outcomes.

The native qualification includes a negative control that mutates event index
3. The comparator must report index 3 and generation 4. A comparator that only
reports a final hash mismatch is insufficient for this contract.

## Native Authority

`test_wt_fault_order_determinism` must pass in debug and release and produce the
same semantic hash:

```text
FAULT_ORDER_DETERMINISM_HASH 4cfe12662b60691f93eda8a27d20e1c978e9bab58c2c144b323750d2f39316bf
```

Its pass record must report 64 orders, eight records, three stale results, one
cancellation, two injected allocation failures, one interrupted publication,
two malformed-input checks, drained shutdown, and first divergent generation
4. The active production qualification runs this authority in both build
configurations.
