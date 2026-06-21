# M5 Page and MIT Meshing Budget Contract

Status: implemented reference-hardware component budgets

## Scope

This contract measures three optimized native batches inside one already
running process:

1. Four content-addressed schema-1 pages, totaling 165,672 encoded bytes, are
   requested through `WtAsyncStorageService`, read from real files, checked
   against the world manifest and SHA-256 identity, opened, and decoded.
2. The four decoded LOD0 pages are used as sample sources for four regular
   16-cubed chunks through the official MIT backend.
3. Seven LOD1 chunks are meshed from deterministic native sphere sources:
   one for each transition orientation and one three-face transition chunk.

Each I/O iteration bypasses `WtStoragePageCache`, so it represents an
application-cache miss. Ten warmup iterations intentionally make this a warm
operating-system filesystem-cache measurement. It does not claim cold-device,
network, antivirus, or arbitrary user-storage latency.

The timers exclude process startup, fixture baking/writing, service open,
Python, build time, and evidence serialization. Peak working set covers the
native benchmark process, including storage, decoded pages, meshing scratch,
and retained mesh capacities.

## Important integration boundary

Schema-1 pages contain 19 samples per axis at the page LOD's cell spacing. A
single higher-LOD page therefore does not contain the half-spacing face
samples required by Transvoxel transition cells, or every unit-offset sample
used by the current gradient calculation.

This benchmark consequently makes two separate claims:

- decoded schema-1 pages directly drive real LOD0 MIT regular meshing;
- the official MIT backend performs real higher-LOD transition meshing when a
  source capable of answering the required fine samples is supplied.

It does **not** claim that one decoded coarse page can independently produce a
seam-correct transition mesh. M5 must resolve transition-sample ownership,
most likely through explicitly loaded fine-neighbor data or a reviewed page
schema change, and then add an integrated storage-to-transition seam budget.
Interpolating absent samples without a seam contract is not accepted.

## Reference host and method

The locked profile is Windows 10.0.19045 x86-64, Intel Family 6 Model 158
Stepping 9, four logical CPUs, 17,066,790,912 physical bytes, Zig 0.16.0, and
the `template_release` configuration.

Run:

```console
python tools/benchmark_m5_pipeline.py
```

The default performs 10 warmup and 101 measured iterations. Native durations
use `std::chrono::steady_clock`; percentiles use nearest rank over sorted
integer nanoseconds. The runner records host identity, Git state, executable
SHA-256, Zig version, workload geometry counts, all raw samples, p50/p95/p99,
maximum, derived per-page/per-chunk p95, peak working set, and the exact budget
profile.

Evidence is written to:

```text
docs/evidence/m5_pipeline_budget_windows_x86_64.json
```

The regular M5 suite executes a three-sample interface smoke check without
enforcing hardware thresholds.

## Initial reference result

The 2026-06-21 reference capture recorded:

```text
I/O + validation + decode batch p50       7,065,700 ns
I/O + validation + decode batch p95      11,645,800 ns
I/O + validation + decode per-page p95    2,911,450 ns
decoded-page regular mesh batch p50      29,793,000 ns
decoded-page regular mesh batch p95      43,287,800 ns
decoded-page regular mesh per-chunk p95  10,821,950 ns
transition mesh batch p50               104,161,700 ns
transition mesh batch p95               139,907,000 ns
transition mesh per-chunk p95            19,986,715 ns
peak process working set                  8,794,112 bytes
```

The locked gates are:

```text
I/O + validation + decode batch p50      <= 20,000,000 ns
I/O + validation + decode batch p95      <= 50,000,000 ns
decoded-page regular mesh batch p50      <= 75,000,000 ns
decoded-page regular mesh batch p95     <= 200,000,000 ns
transition mesh batch p50               <= 200,000,000 ns
transition mesh batch p95               <= 500,000,000 ns
peak process working set                 <= 33,554,432 bytes
```

The p50 gates detect broad steady-state regressions. The larger p95 margins
allow desktop scheduling noise while still rejecting order-of-magnitude
regressions. Budgets are not rewritten automatically from new measurements.

## Remaining M5 measurement work

- define and implement higher-LOD transition sample ownership;
- prove integrated storage-to-transition seam preservation and latency;
- measure Godot render and physics application frame cost;
- lock production collision distance/readiness policy;
- produce binary telemetry and fixed-duration soak evidence.
