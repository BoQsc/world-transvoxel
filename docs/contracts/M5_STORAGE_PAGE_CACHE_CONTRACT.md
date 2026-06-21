# M5 Storage Page Cache Contract

Status: implemented foundation

## Responsibility

`WtStoragePageCache` owns two authoritative storage tiers:

1. validated immutable encoded `wtchunk` byte vectors;
2. lazily decoded immutable `WtChunkPage` sample vectors.

It does not own generated meshes, render payloads, collision payloads, desired
LOD, or Godot resources.

## Identity and stale work

An immutable cache identity is:

```text
(chunk key, source revision, complete-page SHA-256)
```

The indexed lookup key is `(chunk key, source revision)`. Inserting different
bytes under an existing lookup key is an identity conflict and leaves the
resident entry unchanged. This check spans both tiers, including the case where
encoded bytes were evicted while decoded samples remain resident.

`accept_completion()` requires the caller's current nonzero generation token.
If it differs from the asynchronous completion token, the completion is
rejected before page parsing or cache mutation. Failed load completions and
page/key mismatches are also rejected before residency.

## Bounds and accounting

Construction fixes four nonzero bounds:

- encoded entry capacity;
- encoded payload byte capacity;
- decoded entry capacity;
- decoded payload byte capacity.

Entry capacities are at most 65,536. Each tier's payload byte capacity is at
most 1 GiB; individual encoded pages remain bounded by the existing 256 MiB
container limit. A single item larger than its tier's byte budget is rejected
without evicting valid residents.

Encoded residency counts the immutable byte-vector wrapper plus its allocated
capacity. Decoded residency counts the page wrapper plus allocated sample
capacity. Entry metadata and vector bookkeeping are separately bounded by item
capacity and are not included in the payload-byte counters.

## Eviction

Each tier has an independent deterministic least-recently-used order. Hits,
refreshes, and insertions advance a monotonic access sequence. When insertion
or refresh exceeds an item or byte bound, the least-recently-used identity is
evicted; identity order breaks an impossible equal-sequence tie.

Eviction occurs only on explicit cache operations. There is no idle scan.
`erase_key()` removes every source revision for a chunk from both tiers.
`clear()` removes all cache ownership and resets resident counters.

## Ownership after eviction

Cache hits return `shared_ptr<const ...>`. Eviction releases the cache's
ownership but does not invalidate immutable handles already held by in-flight
meshing or application work. Such external ownership is outside cache-resident
byte counters and must remain bounded by the later job graph.

The cache itself is single-owner and not internally synchronized. The storage
or scheduler service must serialize mutation and lookup; immutable handles can
then be transferred to worker jobs.

## Decode behavior

Decoded pages are created lazily from resident encoded bytes. The fixed sample
footprint is checked against the decoded byte budget before allocation.
Container metadata and every finite density/material sample are validated by
the existing `wtchunk` decoder. Decode failure never creates a decoded entry.

## Metrics

Metrics expose:

- accepted, stale, failed, invalid, and identity-conflicting completions;
- encoded and decoded hits/misses;
- insertions, refreshes, evictions, and oversize rejections;
- decode failures;
- current entries and payload bytes per tier.

## Evidence

`test_wt_m5_storage_cache` proves:

- stale, failed, invalid, and zero-generation rejection;
- immutable identity refresh and conflict behavior;
- independent two-entry encoded and decoded LRU eviction;
- item and byte bounds;
- lazy decode and decoded hits;
- explicit two-tier key eviction and clear;
- immutable external handles surviving eviction;
- deterministic metrics and matching debug/release evidence hash:

```text
11b2749ef19124bf73f6f2e287f0cc0da3c877fd9876d3cd42e0d031bd0f740f
```

This completes authoritative page/sample cache ownership. Derived mesh,
render, and collision caches remain the next bounded M5 cache unit.
