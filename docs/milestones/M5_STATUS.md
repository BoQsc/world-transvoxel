# M5 Streaming Production Baseline Status

Status: complete on 2026-06-21 for Windows x86-64, Godot 4.6.3 and 4.7

Implementation revision for the final soak unit:
`6b11c04d831c8a47c4b3c2508e955ef62de6668e`.

## Completed scope

M5 provides:

- bounded asynchronous manifest and content-addressed page loading;
- bounded authoritative page/sample and derived mesh/render/collision caches;
- deterministic multi-viewer desired-set ownership without idle world scans;
- edit-driven generation cancellation, cache eviction, and chunk replacement;
- runtime support-page dependency scheduling and pinning for transition
  meshing;
- reference budgets for native orchestration, page I/O/decode, official MIT
  regular/transition meshing, and real Godot render/physics application;
- versioned, bounded, hash-verified `wttrace` binary telemetry;
- a fixed-duration movement, teleport, underground, vertical, multi-viewer,
  edit, collision-readiness, cancellation, and drain soak.

## Locked final evidence

The reference command:

```console
python tools/run_m5_soak.py
```

produced:

```text
duration                  60,000,003,900 ns
simulated frames          3,178,458
viewer events             4,371,946
edit events               49,664
worker jobs               15,979,240
cancellations             11,951,871
stale results handled     15,520
render applications       7,174,820
collision applications    2,276,746
maximum desired chunks    10
maximum records           10
maximum queued jobs       11
maximum collision queue   7
maximum resources         21
maximum pending readiness 10
maximum readiness         2 frames
rejection/failure paths   0
peak working set          4,612,096 bytes
trace events              3,105
trace bytes               397,752
trace SHA-256             cb67f1075ac2ee4e4f581451884d2bd5800816017e0bc347be3649b95513fbaa
```

Evidence files:

- `docs/evidence/m5_soak_windows_x86_64.json`;
- `docs/evidence/m5_soak_windows_x86_64.wttrace`;
- `docs/evidence/m5_runtime_budget_windows_x86_64.json`;
- `docs/evidence/m5_pipeline_budget_windows_x86_64.json`;
- `docs/evidence/m5_application_budget_godot_4_6_3_windows_x86_64.json`;
- `docs/evidence/m5_application_budget_godot_4_7_windows_x86_64.json`.

The final debug/release build, deterministic trace contract, short soak
interface, full M5-through-M0 suite, and Godot 4.6.3/4.7 integration matrix
pass.

## Exit decision

M5's exit condition is satisfied by the combined evidence:

- the fixed-duration soak proves bounded long-running orchestration, memory,
  queues, cancellation, edits, collision readiness, and complete drain;
- decoded-page face/corner suites prove zero detected LOD seams using the
  official MIT backend;
- optimized native and Godot profiles prove the declared component latency,
  application, collision, and readiness budgets.

The synthetic orchestration soak is not represented as a real full-world Godot
terrain soak. That belongs to production qualification.

## Next boundary

The subsequent production qualification phase proved a
clean installed addon and a real baked-world workflow that streams, renders,
collides, edits, queries, saves, reloads, migrates, and shuts down correctly.

M6 compute acceleration remains optional and is not a production-release
prerequisite. Post-production 0BSD backend qualification remains separate.
