# M5 Multi-Viewer Desired-Set Contract

Status: implemented foundation

## Responsibility

`WtMultiViewerDesiredSet` owns bounded per-viewer chunk demands and their
deterministic global union. Accepted viewer events emit typed added, removed,
and updated deltas for later stream-scheduler application.

It does not freeze production distance bands, LOD radii, collision policy, or
view shape. A caller-side policy produces each viewer's bounded demand list
from its immutable position snapshot. Those numerical policies remain M5
measurement decisions.

## Viewer events

An update contains:

- nonzero viewer ID;
- finite world position;
- nonzero monotonically increasing viewer revision;
- a bounded list of `(chunk key, priority)` demands.

Demand order is irrelevant. Keys must be valid and unique within one viewer.
Stale revisions, duplicate keys, invalid snapshots, and all capacity failures
reject atomically: viewers, the global union, and output delta remain
unchanged.

Removal requires an existing viewer and a revision newer than its last update.
Removal deletes that viewer's revision history. Applications must use a new
logical ID rather than reuse a removed viewer ID for an unrelated lifecycle.

## Union and priority

The global set is sorted by canonical chunk key. For every key it records:

- maximum priority requested by any viewer;
- number of supporting viewers;
- whether any supporting viewer currently requires collision.

Collision demand is aggregated with logical OR. Changing priority, supporter
count, or collision demand emits an updated entry. A newly supported key emits
an addition; loss of the final supporter emits a removal.

The union is a load/prefetch desired set, not the final render-leaf map. Parent
and child demands may coexist when different policies or viewers need them.
Before rendering, selected active leaves still pass the non-overlap and 2:1
rules in `WtLodMap`.

## Bounds

Construction fixes:

- viewer capacity, at most 1,024;
- demand capacity per viewer, at most 65,536;
- total demand capacity, at most 65,536;
- unique desired-chunk capacity, at most 65,536.

Every accepted event rebuilds a candidate union from bounded active demands,
sorts it deterministically, computes a delta, and only then commits. Candidate
failure cannot partially remove or add desired chunks.

## Event-driven behavior

There is no tick or idle update method. Work occurs only for explicit viewer
update/removal events. Read-only access performs binary lookup or returns the
already materialized sorted vector and never rebuilds or scans world state.

An accepted event with unchanged demands may rebuild the bounded candidate but
emits an empty delta. Consumers therefore perform no redundant chunk work.

## Ownership and metrics

The desired set is a single-owner scheduler component and is not internally
synchronized. Metrics expose accepted updates/removals, candidate rebuilds,
demand items processed, delta counts, and rejected events.

## Evidence

`test_wt_m5_multi_viewer` proves:

- deterministic union independent of viewer and demand order;
- max-priority, supporter-count, and collision-demand aggregation;
- typed add/remove/update deltas during movement and removal;
- stale, invalid, duplicate, missing, and every capacity failure path;
- atomic state retention after rejected events;
- parent/child prefetch coexistence;
- 1,000 bounded movement updates;
- 1,000 idle reads with zero rebuilds;
- matching debug/release evidence hash:

```text
65c5397d2e6174c496a6b3ebc06d1547b39cc0c0ca36a1794e5177914c7fe696
```

`WtDesiredSetRuntimeService` now consumes these deltas for bounded scheduler,
application, and cache ownership. Representative workload evidence is recorded
in `M5_REPRESENTATIVE_WORKLOAD_CONTRACT.md`.
