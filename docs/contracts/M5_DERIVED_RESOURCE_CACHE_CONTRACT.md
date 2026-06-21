# M5 Derived Resource Cache Contract

Status: implemented foundation

## Responsibility

`WtChunkResourceCache` owns three independently bounded immutable native
payload tiers:

1. chunk mesh results;
2. render payloads;
3. collision payloads.

These are derived caches. They do not replace authoritative encoded pages,
decoded samples, edit state, or applied Godot resources.

Payload structural validation and resident-byte accounting live separately in
`wt_chunk_resource_payload.*`; cache ownership and eviction remain in
`wt_chunk_resource_cache.*`.

## Identity and generation

The lookup identity is:

```text
(chunk key, generation token, payload tier)
```

Every insertion supplies or contains a nonzero generation and the caller's
current generation. Stale payloads are rejected before validation or cache
mutation.

At most one generation per chunk is resident in each tier. Inserting a newer
current generation explicitly supersedes the older generation in that tier.
Reinserting equal content for the same identity refreshes ownership and LRU
age. Different content for the same identity is rejected as an identity
conflict.

## Structural validation

Mesh validation checks:

- valid chunk key, world origin, transition mask, and LOD relationship;
- per-buffer vertex/index limits and triangle index counts;
- finite vertex positions and normals;
- topology endpoint bounds;
- active/inactive transition-face ownership;
- every index is within its buffer.

Render validation checks key, generation, origin, finite vertices/normals,
triangle index counts, hard limits, and index bounds.

Collision validation checks key, generation, origin, finite face positions,
triangle counts, hard limits, overflow-safe sanitation metrics, and exact
input/output/degenerate/thin accounting.

## Bounds and eviction

Construction fixes nonzero item and payload-byte capacities for every tier.
Item capacities are at most 65,536 and each tier's payload-byte budget is at
most 1 GiB. Resident bytes count payload wrappers plus allocated vector
capacities.

An item larger than its tier budget is rejected without disturbing residents.
Otherwise, insertion and refresh enforce deterministic least-recently-used
eviction independently per tier. Hits, refreshes, and insertions advance a
monotonic access sequence. No idle scan occurs.

`erase_key()` removes all three tier entries for a chunk. `clear()` releases
all cache ownership and resets resident counters.

## Ownership

Hits return `shared_ptr<const ...>`. Eviction releases cache ownership without
invalidating handles already held by worker, application, or transition work.
External handle lifetime is outside resident-byte counters and must be bounded
by the later job graph.

The cache is single-owner and not internally synchronized. The scheduler or
world service serializes lookup and mutation, then transfers immutable handles
to workers.

## Metrics

Metrics expose global stale, invalid, and identity-conflict rejections plus,
for each tier:

- hits and misses;
- insertions and equal-content refreshes;
- LRU evictions;
- generation supersessions;
- oversize rejections;
- current entries and resident payload bytes.

## Evidence

`test_wt_m5_resource_cache` proves:

- stale and structurally invalid rejection for all three tiers;
- item/byte budget enforcement;
- equal-content refresh and conflicting-content rejection;
- independent two-entry LRU eviction;
- generation supersession;
- immutable hit handles surviving eviction;
- cross-tier key eviction and clear;
- deterministic metrics and matching debug/release evidence hash:

```text
842a4104d541f930c88ed3c6bcea6a1a34f83c4725b88cd3da28803a90c17dc8
```

This completes M5's bounded cache foundation. Multi-viewer runtime ownership,
edit replacement, and representative workload contracts now consume these
generation-safe resources.
