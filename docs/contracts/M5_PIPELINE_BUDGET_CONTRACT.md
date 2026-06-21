# M5 Page and MIT Meshing Budget Contract

Status: implemented reference-hardware component budgets

## Scope

This contract measures three optimized native batches inside one already
running process:

1. Seventeen content-addressed schema-1 pages, totaling 704,106 encoded bytes, are
   requested through `WtAsyncStorageService`, read from real files, checked
   against the world manifest and SHA-256 identity, opened, and decoded.
2. The four decoded LOD0 pages are used as sample sources for four regular
   16-cubed chunks through the official MIT backend.
3. One decoded LOD1 coarse page and twelve decoded LOD0 support pages produce
   one real three-face transition chunk through the official MIT backend.

Each I/O iteration bypasses `WtStoragePageCache`, so it represents an
application-cache miss. Ten warmup iterations intentionally make this a warm
operating-system filesystem-cache measurement. It does not claim cold-device,
network, antivirus, or arbitrary user-storage latency.

The timers exclude process startup, fixture baking/writing, service open,
Python, build time, and evidence serialization. Peak working set covers the
native benchmark process, including storage, decoded pages, meshing scratch,
and retained mesh capacities.

## Transition ownership

Schema-1 remains unchanged. Each active coarse transition face owns four
canonical adjacent pages at LOD minus one. `WtChunkPageSampleSource` combines
the primary and support pages, rejects overlap disagreement, and supplies
regular gradients at coarse spacing plus transition gradients at fine
spacing. The ownership and seam proof is defined by
`M5_TRANSITION_PAGE_SOURCE_CONTRACT.md`.

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

## Locked reference result

The 2026-06-21 reference capture recorded:

```text
I/O + validation + decode batch p50      25,615,900 ns
I/O + validation + decode batch p95      39,733,800 ns
I/O + validation + decode per-page p95    2,337,283 ns
decoded-page regular mesh batch p50      33,752,100 ns
decoded-page regular mesh batch p95      82,150,300 ns
decoded-page regular mesh per-chunk p95  20,537,575 ns
page transition mesh batch p50           19,846,400 ns
page transition mesh batch p95           44,272,600 ns
page transition mesh per-chunk p95       44,272,600 ns
peak process working set                  9,293,824 bytes
```

The locked gates are:

```text
I/O + validation + decode batch p50      <= 60,000,000 ns
I/O + validation + decode batch p95     <= 150,000,000 ns
decoded-page regular mesh batch p50     <= 100,000,000 ns
decoded-page regular mesh batch p95     <= 400,000,000 ns
page transition mesh batch p50           <= 75,000,000 ns
page transition mesh batch p95          <= 400,000,000 ns
peak process working set                 <= 33,554,432 bytes
```

The p50 gates detect broad steady-state regressions. The larger p95 margins
allow desktop scheduling noise while still rejecting order-of-magnitude
regressions. Budgets are not rewritten automatically from new measurements.

## Remaining M5 measurement work

- measure Godot render and physics application frame cost;
- lock production collision distance/readiness policy;
- produce binary telemetry and fixed-duration soak evidence.
