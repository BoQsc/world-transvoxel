# CPU Causal Trace Contract

## Purpose

The CPU causal trace is an opt-in diagnostic for attributing relocation and
edit latency across the authoritative terrain pipeline. It complements the
aggregate `get_runtime_metrics()` snapshot and the deterministic M5 binary
trace; it does not replace either contract.

## API

`WorldTransvoxelTerrain` exposes:

- `begin_cpu_causal_trace()` to clear and enable the native ring.
- `get_cpu_causal_trace_events(first_sequence, maximum_events)` to read a
  bounded sequence delta.
- `end_cpu_causal_trace()` to append the final event and disable recording.

The configured `trace_event_capacity` is the only native retention bound.
Tracing is disabled by default. Disabled terrain behavior is unchanged apart
from cheap enable checks at instrumented boundaries.

## Event Identity

Chunk events carry the authoritative `WtChunkKey` and `WtGenerationToken`.
Viewer events use the submitted viewer revision as `cause_id`. Edit events use
the committed world revision as `cause_id`. Storage dependencies retain the
consumer generation that requested them. These fields allow a consumer to
follow one generation through:

1. viewer plan or edit replacement demand;
2. storage request, worker start, worker finish, and completion consumption;
3. sample and mesh execution;
4. mesh completion and publication queue/pop;
5. front-end publication processing; and
6. actual Godot render or collision sink application.

Every duration uses `std::chrono::steady_clock`. Event order is the serialized
order in the thread-safe ring, while `elapsed_ns` records the observation time.
The ring reports retained sequence bounds and overwritten event count so a
consumer cannot mistake a partial trace for a complete causal history.

## Observation Rules

- Recording must never change scheduling, queue priority, generation, LOD,
  storage, publication, render, or collision decisions.
- The native ring performs no file I/O and allocates its slots only when
  tracing begins.
- Consumers own JSON serialization and must report their own capture and write
  costs separately from terrain timing.
- Performance conclusions require a traced/untraced comparison on the same
  executable, scenario, affinity, and runtime policy.

## Validation

`test_wt_causal_trace` verifies disabled-default behavior, bounded overwrite,
sequence deltas, chunk/generation identity, and stop semantics in debug and
release. The affected application, async storage, page-meshing, lifecycle,
streaming, and production LOD suites must also pass.

The pinned `f30818b` authority line has two pre-existing `test_m5.py` expected
hash mismatches: page transition emits `65d54a1208db2e8f`, and page meshing
runtime emits `8b5ff828...`; clean `f30818b` reproduces both. They are not
introduced or waived by this trace contract.
