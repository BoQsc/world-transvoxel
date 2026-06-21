# M5 Native Runtime Budget Contract

Status: implemented reference-hardware orchestration budget

## Scope

This contract measures the deterministic representative workload defined by
`M5_REPRESENTATIVE_WORKLOAD_CONTRACT.md` in the optimized native test binary.
One measured run includes:

- 118 viewer demand events;
- 8 committed edit events;
- 106 simulated application frames;
- 526 synthetic sample/mesh job completions;
- desired-set union and runtime-delta application;
- generation replacement and stale-result handling;
- native render/collision payload construction;
- derived cache insertion and release;
- bounded render/collision application and readiness tracking.

The timer surrounds one complete scenario inside the already-running native
process. Process startup, Python, build time, and evidence serialization are
excluded. Peak working set covers the native benchmark process.

This is an orchestration and payload-preparation budget. It does not include
filesystem page latency, full Transvoxel chunk meshing, Godot `ArrayMesh`
creation, RenderingServer/PhysicsServer work, scene-tree cost, or actual frame
pacing. Page and native MIT meshing costs are measured separately by
`M5_PIPELINE_BUDGET_CONTRACT.md`; real engine application is measured by
`M5_GODOT_APPLICATION_BUDGET_CONTRACT.md`.

## Reference host

The first locked profile is:

```text
platform             Windows 10.0.19045 x86-64
CPU                  Intel Family 6 Model 158 Stepping 9
logical CPUs         4
physical memory      17,066,790,912 bytes
compiler driver      Zig 0.16.0
configuration        template_release
```

Platform support is still claimed only for the existing Windows x86-64 matrix.
Other hosts may record measurements, but do not inherit this reference profile
without explicit evidence and a new named budget profile.

## Method

Run:

```console
python tools/benchmark_m5_runtime.py
```

The default run performs 10 warmup scenarios followed by 101 measured
scenarios. Durations use `std::chrono::steady_clock`. Percentiles use nearest
rank over sorted integer nanosecond samples. The Python runner records:

- host CPU, logical CPU count, physical memory, OS, and architecture;
- Git revision and dirty description;
- release executable SHA-256 and Zig version;
- all raw duration samples;
- p50, p95, p99, maximum, derived simulated-frame p95, and peak working set;
- the exact applied budget profile.

Evidence is written to:

```text
docs/evidence/m5_runtime_budget_windows_x86_64.json
```

The executable also exposes a three-sample benchmark-interface smoke check in
the regular M5 validation suite. The smoke check verifies output and execution,
not performance thresholds.

## Locked reference result

The 2026-06-21 reference evidence records:

```text
scenario p50                 1,123,300 ns
scenario p95                 2,916,700 ns
scenario p99                 3,021,800 ns
scenario maximum             3,045,500 ns
simulated-frame p95             27,517 ns
peak process working set     3,760,128 bytes
```

The locked gates are deliberately above the captured values to tolerate normal
desktop scheduling noise while still detecting material regressions:

```text
scenario p50                <= 3,000,000 ns
scenario p95               <= 20,000,000 ns
simulated-frame p95           <= 190,000 ns
peak process working set    <= 8,388,608 bytes
```

The p95 scenario gate is the primary tail-latency guard. The p50 gate prevents
a broad steady-state regression from hiding behind a permissive tail margin.
The frame value is the scenario p95 divided by the fixed 106 simulated frames;
it is not a Godot frame-time claim.

## Failure behavior

The benchmark fails if:

- the native benchmark protocol or sample count is wrong;
- the selected profile does not match platform and architecture;
- any locked measurement exceeds its limit;
- the evidence file cannot be written.

Budgets are not silently rewritten from new measurements. Changing a limit,
workload shape, percentile method, reference profile, or measured scope
requires an explicit reviewed update to this contract and budget JSON.

## Remaining M5 measurement work

This completes reference-hardware measurement for native runtime orchestration.
Still required before M5 completion:

- binary telemetry traces and fixed-duration soak evidence.
