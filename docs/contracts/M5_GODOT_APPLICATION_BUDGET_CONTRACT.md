# M5 Godot Application Budget Contract

Status: complete with locked Windows x86-64 evidence

## Scope

This contract measures the main-thread boundary that was intentionally
excluded from the native M5 runtime and pipeline budgets:

- conversion of immutable native render payloads into Godot packed arrays;
- `ArrayMesh` surface creation and `MeshInstance3D` creation/replacement;
- conversion of collision faces into `PackedVector3Array`;
- `ConcavePolygonShape3D` creation and `StaticBody3D` creation/replacement;
- bounded `WtChunkApplicationService` queue consumption and readiness updates;
- render/collision resource teardown.

The timer surrounds direct calls to the same application service and Godot
sinks used by `WorldTransvoxelTerrain::_process`. Automatic terrain application
is disabled during the benchmark; each timed bounded call is followed by one
real Godot frame so RenderingServer and PhysicsServer can advance between
logical application frames. Payload meshing and collision sanitation happen
before timing. Process startup, GDScript orchestration, idle frame wait,
filesystem I/O, worker meshing, GPU drawing, and broad scene-tree simulation
are excluded. Peak working set covers the complete headless Godot process.

## Workload

One run replaces a burst of 32 render resources and 16 required collision
resources. The production default per-frame budgets are:

```text
render applications      4
collision applications   2
```

The queues therefore drain in exactly eight application frames. Every frame
must process no more than its configured budget. At the eighth frame:

- both queues are empty;
- all 32 expected generations are fully ready;
- exactly 32 render and 16 collision resources are owned;
- maximum render and collision queue latency is eight application ticks.

One cold creation run is recorded separately. Warmup and measured runs replace
the same keys with newer generations, proving that replacement does not create
duplicate chunk nodes. Final teardown removes all resources and records its
main-thread duration.

`WtM5ApplicationBenchmarkFixture` exists only to create deterministic immutable
payloads and time the production application boundary. GDScript parses command
arguments, sequences runs, checks results, and prints samples; no
efficiency-sensitive terrain loop is implemented in GDScript.

## Collision and readiness policy

The M5 production CPU baseline locks the existing default collision policy:

```text
activate collision at distance <= 96 base units
retain collision through distance <= 128 base units
collision thin-triangle ratio squared = 1e-12
```

Visual readiness is independent. A chunk is fully ready when visual output is
ready and collision output is either ready or not required. Enabling collision
resets only collision readiness. Disabling collision makes the chunk fully
ready without waiting for or applying an obsolete collision payload.

These distances are addon defaults, not game-specific promises. A project may
configure a different validated policy, but it must preserve hysteresis,
bounded application, and independent readiness.

## Matrix and budget profiles

The benchmark runs the optimized addon in pinned headless Godot 4.6.3 and 4.7
on the Windows x86-64 reference host. Each engine has an explicit profile in
`tests/performance/m5_application_budget.json`; profiles are not inherited by
other engines, platforms, architectures, or hardware.

The measured fields are cold scenario/frame cost, replacement scenario
percentiles, maximum-frame percentiles, render and collision sink
percentiles, teardown duration, readiness latency, and peak process working
set. A profile mismatch or exceeded limit fails. Limits are reviewed contract
values and are never rewritten automatically from a new capture.

Run either profile with:

```console
python tools/benchmark_m5_application.py --engine-version 4.6.3
python tools/benchmark_m5_application.py --engine-version 4.7
```

`scripts/test_m5.py` performs a three-sample interface and correctness smoke
run on both engines before the existing complete Godot integration matrix.

## Locked evidence

Both optimized captures use 101 measured runs after 10 warmup runs:

| Measurement | Godot 4.6.3 | Godot 4.7 |
| --- | ---: | ---: |
| replacement scenario p50 | 64,345,000 ns | 69,227,800 ns |
| replacement scenario p95 | 148,857,000 ns | 97,189,300 ns |
| maximum frame p95 | 55,008,900 ns | 28,519,200 ns |
| render sink p95 | 12,825,200 ns | 18,189,700 ns |
| collision sink p95 | 134,687,500 ns | 86,355,400 ns |
| cold scenario | 62,499,700 ns | 84,268,500 ns |
| teardown | 197,300 ns | 158,300 ns |
| peak process working set | 101,085,184 bytes | 106,741,760 bytes |
| maximum readiness | 8 frames | 8 frames |

The evidence files are:

- `docs/evidence/m5_application_budget_godot_4_6_3_windows_x86_64.json`;
- `docs/evidence/m5_application_budget_godot_4_7_windows_x86_64.json`.

Both captures identify implementation revision
`eaa47b37a130a7e23c98865661ca250424d275b5`. The complete debug/release build,
both budget profiles, the M5 smoke interfaces, and the M5-through-M0 regression
and Godot integration matrix pass.

Collision-shape replacement dominates this workload. The locked 4/2
application budgets bound that cost and intentionally favor frame spreading
over immediate readiness for a 32/16 burst. The completed binary telemetry and
fixed-duration soak contract now combines with this profile to close M5.
