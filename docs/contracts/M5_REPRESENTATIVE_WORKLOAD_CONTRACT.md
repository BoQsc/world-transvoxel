# M5 Representative Workload Contract

Status: implemented deterministic functional baseline

## Responsibility

This unit connects multi-viewer desired-set deltas to bounded runtime ownership
and proves representative movement/edit behavior before hardware performance
budgets are selected.

`WtDesiredSetRuntimeService` consumes canonical `WtDesiredSetDelta` events and
coordinates:

- scheduler record/job creation for additions;
- queued-job and future-stage reprioritization for updates;
- collision-demand changes without remeshing unchanged geometry;
- scheduler/application record release for removals;
- page/sample and derived-resource cache release for removals.

It does not choose viewer demand shapes, execute storage or meshing workers,
create Godot resources, or measure wall-clock performance.

## Desired collision demand

Each `WtViewerChunkDemand` now carries `collision_required`. The global desired
union records the logical OR across supporting viewers, independently from its
maximum scheduling priority and supporter count.

A collision-demand change emits an updated desired entry even when key,
priority, and supporter count are unchanged. Runtime application preserves
visual readiness while `false -> true` resets collision readiness until a
current-generation collision payload is applied.

## Runtime delta preflight

Construction fixes a maximum total change count. Before mutation, one delta is
validated for:

- canonical strictly increasing keys in each added/removed/updated list;
- valid keys and nonzero supporter counts;
- no key appearing in multiple change lists;
- matching scheduler/application state for removals and updates;
- absent scheduler/application state for additions;
- sufficient scheduler and application record capacity after removals;
- sufficient current scheduler job capacity for every addition.

Expected validation, state, record, and job-capacity failures reject before
record, queue, readiness, or cache mutation.

Desired-set and runtime services share one owner. Their construction bounds
must be configured coherently so an accepted desired event cannot exceed the
runtime consumer's capacities.

## Removal and reprioritization

`WtStreamScheduler::forget_chunk()` removes the bounded record and discards
queued jobs and queued completions for that key. Already running jobs become
stale because no matching record exists; globally monotonic generation tokens
prevent a later reload from accepting the old result.

This differs from `cancel_chunk()`, which intentionally retains a cancelled
record for callers that still own that key.

`reprioritize_chunk()` updates the record and every queued job for its current
generation. If the sample job is already running, its later mesh job inherits
the new record priority.

## Deterministic workload

`test_wt_m5_workload` runs one event-driven synthetic runtime through:

1. continuous and fast-vehicle horizontal movement;
2. a 512-chunk teleport;
3. negative-coordinate underground traversal;
4. high positive vertical traversal;
5. two independently moving viewers;
6. eight edits committed while viewers move.

The fixture uses five demanded chunks per viewer. The center chunk requires
collision and has the highest priority. Worker, render, and collision work are
processed with explicit per-frame budgets; no idle world scan drives the
simulation.

The locked run contains 118 viewer events, 8 edit events, 106 simulated frames,
and 526 sample/mesh worker jobs. Its observed maxima are:

```text
desired chunks             10 / 32
scheduler records          10 / 32
queued jobs                10 / 128
queued render payloads     29 / 128
queued collision payloads   5 / 128
derived cache entries      30 / 96 across three 32-entry tiers
readiness latency          10 simulated frames
```

The run produces no queue, record, desired-set, runtime-delta, or edit
replacement rejection. Removed chunks do not accumulate records, collision
readiness recovers after movement, stale edited generations are rejected, and
all final desired chunks are fully ready.

## Evidence

The test additionally proves:

- queued additions are reprioritized without a new generation;
- removal discards queued jobs/completions and releases scheduler/application
  ownership;
- invalid, noncanonical, missing-state, record-capacity, job-capacity, and
  change-capacity deltas reject without partial mutation;
- matching debug/release output hash:

```text
c5bdf6b8896f0a5e4271c5aeab2e8f552e7b776bccc66e9082595467ff90b2a3
```

These are deterministic functional bounds, not production hardware budgets.
The fixture uses immediate synthetic worker completions and native payload
builders; it does not measure filesystem latency, real terrain meshing cost,
Godot frame time, physics-server cost, seam geometry, or percentile wall-clock
latency. Native orchestration wall-clock and process-memory budgets are now
defined separately in `M5_RUNTIME_BUDGET_CONTRACT.md`.
