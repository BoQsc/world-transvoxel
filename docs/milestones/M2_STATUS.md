# M2 Status: Chunk and LOD Ownership

Status: complete

Completed: 2026-06-20

Validated platform: Windows x86-64

## Result

The native addon now has bounded chunk, LOD, meshing, and worker-boundary
contracts. A validated LOD map determines transition ownership before meshing;
the coarser leaf owns every transition toward a finer face. Generation tokens
make cancellation and supersession explicit, and stale completions cannot
change current chunk state.

The deterministic chunk mesher builds regular and transition buffers from the
M1 official MIT backend. Vertices remain local to a signed 64-bit world origin,
and canonical source-edge interpolation keeps neighboring chunks aligned
without converting large world coordinates to floating point.

The normative definition is
[`docs/contracts/M2_CHUNK_LOD_CONTRACT.md`](../contracts/M2_CHUNK_LOD_CONTRACT.md).

## Native evidence

Command:

```console
python scripts/test_m2.py
```

The M2 suite covers:

- signed keys, negative parent mapping, non-overlap, and 2:1 enforcement;
- deterministic transition ownership, including two-face ownership corners;
- priority ordering, typed sampling/meshing stages, cancellation, queue
  overflow, viewer revisions, and stale-result rejection;
- 1,000 repeated supersession cycles with bounded records and queues;
- three same-LOD closed galleries, including both signed coordinate extremes
  at the maximum supported LOD;
- all six transition directions and exact full/half contour agreement;
- all 12 signed two-face LOD-edge orientations;
- all eight signed three-face LOD-corner orientations plus a translated
  negative-coordinate/world-origin corner;
- buffer bounds, index validity, vertex use, failure clearing, and invalid
  meshing input.

Closed sphere galleries require every undirected triangle edge to have exactly
two incident triangles. This detects cracks, T-junctions, duplicate surfaces,
and non-manifold transition overlap in the tested worlds.

Locked aggregate hash:

```text
chunk mesh  4e436f833896a1bc
```

Debug and optimized release configurations produce the same M1 and M2 hashes.

The rebuilt debug and release GDExtension binaries also passed editor and
runtime load tests under Godot 4.6.3-stable and Godot 4.7-stable.

## Scope boundary

M2 proves native chunk topology, LOD ownership, bounded scheduling state, and
deterministic seam closure for the recorded galleries. It does not yet create
Godot mesh or physics resources, apply results on the main thread, sanitize
collision geometry, or run a moving-viewer scene. Those finite gates belong to
M3.

M3 through M5 subsequently completed. Production qualification is the current
active phase.
