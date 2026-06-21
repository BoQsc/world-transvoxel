# M5 Binary Telemetry and Soak Contract

Status: implemented; reference-duration evidence pending

This contract closes the final measurement unit of the M5 streaming production
baseline. It defines a bounded binary runtime trace and a repeatable
fixed-duration orchestration soak. It does not replace the already locked real
page/meshing, seam, or Godot application measurements.

## Trace ownership

Native telemetry owns trace construction and validation:

```text
addons/world_transvoxel/src/telemetry/wt_runtime_trace.h
addons/world_transvoxel/src/telemetry/wt_runtime_trace.cpp
```

`WtRuntimeTraceWriter` receives a construction-time event capacity. Appending
past that capacity fails with `CapacityExceeded`; events are never silently
dropped or stored in an unbounded queue.

The writer and reader use the common hash-verified container with `WTTRACE`
magic. Trace schema 1 has two required sections:

```text
META    fixed run metadata
EVNT    fixed-width checkpoint and final events
```

Unknown optional sections are skipped. Unknown required container features,
future major schemas, corrupt hashes, invalid bounds, non-monotonic records,
missing final records, and source-revision mismatches fail cleanly.

## Schema 1 layout

The 80-byte `META` payload is:

```text
schema_major:u16
schema_minor:u16
event_size:u16
reserved:u16
source_revision:u64
target_duration_ns:u64
actual_duration_ns:u64
frame_count:u64
event_count:u64
event_capacity:u64
sample_period_frames:u64
seed:u64
reserved:u64
```

The `EVNT` section begins with:

```text
schema_major:u16
schema_minor:u16
event_size:u16
reserved:u16
event_count:u64
```

Each event is exactly 128 bytes:

```text
sequence:u64
elapsed_ns:u64
frame:u64
kind:u16
flags:u16
reserved:u32
desired_chunks:u64
scheduler_records:u64
queued_jobs:u64
queued_completions:u64
queued_render:u64
queued_collision:u64
resource_entries:u64
pending_readiness:u64
viewer_events:u64
edit_events:u64
stale_results:u64
total_rejections:u64
```

Schema 1 kinds are `Checkpoint` and `Final`. Sequence numbers are contiguous
from zero; elapsed time and frame number are monotonic; the only final event is
the last event and exactly matches the metadata duration and frame count.
Flags and reserved fields are zero.

No wall-clock timestamp, string, native structure image, pointer-sized field,
Godot `Variant`, or backend topology data appears in the trace. This makes
trace identity independent of filesystem timestamps and compiler layout.

Limits:

- at most 262,144 events per trace;
- at most 65,536 events in the M5 soak writer;
- common-container section and file limits remain authoritative;
- event sampling defaults to once per 1,024 simulated frames.

## Fixed-duration workload

`test_wt_m5_soak` runs the existing bounded M5 multi-viewer/edit/application
fixture until monotonic elapsed time reaches the requested duration, then
removes the optional second viewer and drains all queues.

The deterministic motion pattern repeatedly includes:

- ordinary movement and fast vehicle movement;
- discontinuous teleports;
- underground and vertical traversal;
- a second independently moving viewer;
- second-viewer addition and removal;
- edits every 64 simulated frames;
- collision-required center chunks;
- generation cancellation and stale-result rejection.

The soak is event driven: each viewer change, viewer removal, and edit is an
explicit input event. It does not add an idle full-world scan.

The reference command is:

```console
python tools/run_m5_soak.py
```

It runs an optimized 60-second workload, samples native process peak working
set, validates the emitted trace by reopening it, applies the checked-in
reference-host budget, and writes:

```text
docs/evidence/m5_soak_windows_x86_64.json
docs/evidence/m5_soak_windows_x86_64.wttrace
```

The normal M5 suite runs a short interface smoke test. The 60-second evidence
capture is explicit so routine tests remain finite and fast.

## Acceptance

The soak itself must prove:

- actual duration is at least the requested duration;
- final desired, scheduler, and application ownership agrees;
- every final chunk has required visual and collision readiness;
- final worker, render, collision, completion, and readiness queues drain;
- desired chunks, records, queues, resources, and pending readiness remain
  within construction-time bounds;
- maximum readiness is at most 32 simulated frames;
- all desired-set, scheduler, application, runtime, and edit rejection/failure
  counters remain zero;
- cancellation, stale-result handling, render application, collision
  application, and edit replacement all occur;
- peak working set and trace size remain within the reference profile;
- the binary trace reopens with all hashes and semantic invariants valid.

M5's full exit is a composition of locked evidence:

- this soak proves long-running orchestration, memory, queue, cancellation,
  edit, and collision-readiness behavior;
- `M5_PAGE_MESHING_RUNTIME_CONTRACT.md` and the page transition suite prove
  real decoded-page official MIT meshing and zero detected LOD seams;
- `M5_RUNTIME_BUDGET_CONTRACT.md` and `M5_PIPELINE_BUDGET_CONTRACT.md` prove
  optimized native orchestration and page/meshing latency;
- `M5_GODOT_APPLICATION_BUDGET_CONTRACT.md` proves real engine render and
  physics application cost and readiness.

The synthetic soak does not claim to remeasure every real meshing or Godot
operation inside its timed loop. M5 completes only when all of the above
evidence remains passing together.
