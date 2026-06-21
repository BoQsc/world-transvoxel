# M5 Godot Application Budget Contract

Status: implemented; clean reference-hardware capture pending

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

## Completion gate

This unit is complete only after:

- debug and release addon builds pass;
- both engine smoke runs pass;
- clean 101-sample evidence is committed for both engine profiles;
- the complete M5-through-M0 regression suite passes;
- the roadmap and implementation charter identify binary telemetry and soak
  evidence as the next active unit.
