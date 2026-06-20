# M1 Status: Exact Native Cell Backend

Status: complete

Completed: 2026-06-20

Validated platform: Windows x86-64

## Result

The official MIT backend now implements the project-owned native cell contract
for regular and transition cells. It uses the pinned official lookup data
directly inside the isolated MIT adapter; official table types and class IDs do
not cross project headers.

Implemented behavior:

- negative density is solid and equality with the isovalue is outside;
- published regular and transition case-bit ordering;
- float edge interpolation at configurable isovalue;
- categorical `uint16_t` material selection from the solid endpoint;
- interpolated, normalized world-space gradients;
- nonzero-width transition prisms;
- all six right-handed transition orientations;
- official inverse transition winding;
- exact-zero-area triangle removal;
- fixed 12-vertex/36-index capacity without heap allocation;
- caller-owned or reusable thread-local scratch;
- explicit invalid-input statuses and cleared failed outputs.

The normative definition is
[`docs/contracts/M1_CELL_BACKEND.md`](../contracts/M1_CELL_BACKEND.md).

## Exhaustive native evidence

Command:

```console
python scripts/test_m1.py
```

The native test runs:

- 256/256 regular cases;
- 512/512 transition cases;
- 6/6 transition orientations per case;
- 3,072 oriented transition calls per configuration;
- debug and optimized release configurations;
- repeated-output determinism, bounds, normals, materials, winding anchors,
  orientation equivalence, invalid inputs, exact-isovalue degeneracy, and
  thread-local scratch reuse.

Locked aggregate FNV-1a hashes:

```text
regular     21294cb1188f4c45
transition  94e9e05bd1719331
```

Debug and release produced the same hashes.

## Godot compatibility evidence

The rebuilt debug and release GDExtension binaries loaded successfully under:

```text
Godot 4.6.3-stable  7d41c59c4
Godot 4.7-stable    5b4e0cb0f
```

The repository validator also passed with the official MIT source hash,
godot-cpp revision, license boundary, normalized PE timestamps, and source-size
limits intact.

## Locked M1 decisions

- Runtime density/isovalue: `float`, default isovalue zero.
- Runtime material: categorical `uint16_t`, selected from the solid endpoint.
- Runtime normals: interpolated world-space density gradients.
- Default chunk baseline: 16 cells per axis.
- Meshing sample baseline: 19 samples per axis (`-1..17`) for cell corners and
  central-difference gradient neighbors.

Persistent encodings remain an M4 decision.

## Claim boundary

M1 proves the isolated official MIT cell backend contract. It does not yet
prove multi-cell vertex reuse, chunk ownership, same-LOD seams, LOD transition
seams, LOD corners, scheduling, rendering, collision, editing, persistence, or
streaming. Those gates remain finite roadmap work beginning with M2.

The next active milestone is M2: chunk and LOD ownership.
