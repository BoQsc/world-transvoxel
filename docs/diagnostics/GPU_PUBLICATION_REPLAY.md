# GPU Publication Replay

The production selector can be tested without launching Godot. This is an
isolated CPU query benchmark, not a GPU or gameplay benchmark.

`tests/fixtures/gpu_publication_cohorts_20260830.json` contains eight successful
inspection snapshots from the integration game's G23 moving-player route.
It retains all selector input keys, successful boundary lookups, and expected
replacement, retirement, and mask-wait sets. Other lookups are absent. The
source capture hash is recorded; application readiness and mesh geometry are
not included. Failed inspections must not be treated as complete graphs.

Build `test_wt_publication_policy` with the normal debug/release native targets.
The default test compares publication-region selection with an independent
integer all-pairs oracle: 250 seeded random cases and 30 coordinate-limit cases.
The M3 runner also replays every retained cohort in both configurations.
Coverage regressions additionally check 180 seeded mixed-LOD partitions under
full, clipped, and sparse authority predicates, with shuffled inputs, missing
children, duplicate keys, overlapping ancestors, same-key retirement, invalid
LODs, empty sets, and coordinate-limit cases.

For a timing report from the repository root:

```text
python tools/benchmark_gpu_publication.py --capture tests/fixtures/gpu_publication_cohorts_20260830.json --binary build/native-tests/test_wt_publication_policy.template_debug.x86_64.exe --output artifacts/publication-replay-new.json
```

The runner refuses to overwrite evidence. It records the input and executable
hashes, exact output comparisons, and 50 warmup-excluded query timings by default.
Use the same build configuration, CPU affinity, and idle hardware conditions for
comparisons. Tests enforce equality, not a machine-dependent timing threshold.

## 2026-08-31 Finding

A trial replacing the unsafe-retirement scan with six adjacent hierarchy
queries reproduced every retained set but made the four larger queries slower:
1.698/0.743/2.185/3.931 ms became 2.361/1.245/3.239/6.324 ms. That trial was
removed. The production selector and membership rules are unchanged.

The retained API change instead avoids serializing ready members into Godot
dictionaries when another member is still waiting. All readiness, boundary,
coverage, generation, and priority checks still run. Complete inventories are
serialized only for `READY`; waiting responses still expose no partial inventory.
This saves discarded work but does not establish smooth movement, fast edits,
or GPU completion. Integration evidence is retained under
`docs/evidence/tqp64_gpu_publication_query_20260831` in the integration game.

## Indexed Coverage Follow-Up

Default-off `get_gpu_resident_render_activation_cohort(identity, true)` timing
separates selection, authoritative coverage, member readiness, priority requests,
and response construction. The integration game's `--gpu-stage-timing` records
these nested CPU stages. Do not add them to their enclosing query duration.

Live measurement found coverage was repeatedly scanning every replacement pair
and all replacements at each recursive child. The authoritative coverage check
now uses the existing dyadic hierarchy index for ancestor and descendant lookup.
It still rejects overlap and requires every authoritative child of a retirement
to be covered. World clipping is unchanged. Collision retirement and generation
publication rules, selector membership, and scheduling priorities are unchanged.

The replay now also compares coverage against a separate bounds-based all-pairs
oracle under the G23 world envelope (X/Z 0..2048, Y -128..128). It times both
implementations independently; this reference timing is not GPU performance.
Regions with no retirements are not applicable to the production coverage call.
The live gameplay gate still fails: this optimization is not release acceptance.
Integration evidence: `docs/evidence/tqp64_gpu_coverage_index_20260831`.
