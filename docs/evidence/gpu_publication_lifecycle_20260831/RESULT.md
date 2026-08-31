# GPU Publication Lifecycle

Status: focused correctness fixes; full GPU gameplay qualification still fails.

## Changes

- Check native visual-generation ownership when the frontend application record
  is missing or no longer matches. Retired generations are stale, not pending.
- Exclude known collision-only records from GPU visual candidate selection.
  Shared replacement queues are not a visual coverage inventory. Missing
  application records are preserved and continue to block incomplete coverage.
- Preserve reciprocal same-LOD publication dependencies on retained geometry.
  Zero transition masks in the desired layout alone do not prove the old
  neighboring geometry is compatible. No coverage or collision guard is relaxed.
- Export the exact `visual_candidates` inventory for diagnostic replay.
- Keep queued collision-topology repairs while a chunk remains explicitly
  required for collision, even when it has left the visual LOD plan. Previously
  that repair was discarded, leaving no jobs and a permanently unready chunk.

## Evidence

Normal-profile debug and release builds pass `test_wt_m3_application`,
`test_wt_publication_policy`, and `test_wt_production_streaming`. The publication
oracle includes 250 randomized cases, 30 coordinate-limit cases, and 180
partitions across three authorities and six mutations. These are focused tests,
not a claim that the entire authority suite passed.

The interrupted mask-only optimization failed 13 M3 assertions before correction:
twelve positive/negative face coarsening cases and retained parent grouping.
The strengthened tests pass with reciprocal retained-boundary dependencies.

The integration game's `gpu_resident_application_readiness_smoke.gd` proves two
pending and two stale native/frontend lifecycle cases without compute readback.
Its `gpu_collision_only_publication_smoke.gd` exercises collision-only children
overlapping visual parents during a real rendered relocation, checks exact
candidate inclusion/exclusion, and requires final publication drain and a clean
coverage audit. Its GPU capacity explicitly holds old plus new inventories.
After candidate filtering, a D3D12 run exposed two permanently unready
collision-only records with no queued jobs. Correcting the retry role check
passes three Vulkan and three D3D12 relocations, with seven collision-only
candidate exclusions in each and complete final drain. This is a bounded
integration regression, not a hardware-wide or full-world guarantee.

The real G23 failing-before startup snapshot contained collision-only key
`(18,-1,14)/LOD0` and visual parent `(9,-1,7)/LOD1` in one selected inventory.
Coverage correctly rejected that overlap with empty work queues. After filtering,
the route starts, but movement and relocated-edit acceptance still fail. Native
ownership, exact masks, CPU local physics, and atomic publication remain required.

Downstream `docs/evidence/tqp64_gpu_publication_lifecycle_20260831/` retains the
raw startup trace, gameplay reports, focused logs, and final exact artifact pin.
The inherited M5 wrapper fingerprint issue is not resolved by these tests.
