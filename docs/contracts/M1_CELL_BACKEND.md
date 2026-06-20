# M1 Native Cell Backend Contract

Status: normative for M1 and later backend implementations

## Scope and authority

This contract defines the project-owned behavior shared by every cell meshing
backend. The production implementation is the isolated official MIT backend.
Official table types, equivalence classes, reuse codes, and lookup arrays do
not cross this interface.

The primary references are Eric Lengyel's *Voxel-Based Terrain for Real-Time
Virtual Simulations*, sections 3.1.1, 3.3, 4.3, and 4.5, and the official
`Transvoxel.cpp` pinned at revision
`51a494f03c5b024cd153b596bcc7152eb3cc93a6`. The dissertation fixes the sign,
case-bit, endpoint-order, interpolation, winding, and transition-sample rules.

## Scalar, coordinate, and winding conventions

- Runtime cell densities and isovalues are IEEE-754 `float` values.
- A sample is solid/inside exactly when `density < isovalue`.
- Equality with the isovalue is empty/outside. The default isovalue is zero.
- Positions use a right-handed Cartesian coordinate system.
- Triangle indices are counterclockwise when viewed from the front/outside.
- Interpolation from endpoint A to endpoint B is
  `alpha = (isovalue - density_a) / (density_b - density_a)` and
  `position = A * (1 - alpha) + B * alpha`.
- The table-defined endpoint order is retained. Alpha is clamped to `[0, 1]`
  only to contain floating-point roundoff.

Regular corner indexes are:

```text
6-------7
/|      /|
4-------5 |
| 2-----|-3
|/      |/
0-------1
```

Equivalently, corner `i` is `(i bit 0, i bit 1, i bit 2)`. Case bit `i` is set
when corner `i` is solid.

## Transition cell conventions

The full-resolution face contains nine samples:

```text
6---7---8
|   |   |
3---4---5
|   |   |
0---1---2
```

The half-resolution face aliases the same scalar/material/gradient samples at
its four corners: `9=0`, `A=2`, `B=6`, and `C=8`. The aliases have distinct
positions on the opposite face, so a transition cell has real nonzero width.

Transition case bits use the published order:

```text
bit:     0 1 2 3 4 5 6 7 8
sample:  0 1 2 5 8 7 6 3 4
```

The full-resolution face origin is sample 0. Full-resolution sample spacing
sets the local U/V scale. `transition_width` sets the distance to the
half-resolution face. Orientation names specify the local W direction from
the full-resolution face toward the half-resolution face.

All six mappings are right-handed (`U cross V = W`), so orientation does not
change table winding:

| Orientation | U | V | W |
| --- | --- | --- | --- |
| `PositiveX` | +Y | +Z | +X |
| `NegativeX` | +Y | -Z | -X |
| `PositiveY` | +Z | +X | +Y |
| `NegativeY` | +Z | -X | -Y |
| `PositiveZ` | +X | +Y | +Z |
| `NegativeZ` | +X | -Y | -Z |

The official transition class high bit reverses each triangle. No class ID or
class-inversion flag appears in project-owned outputs.

## Vertex attributes

- Callers provide a finite world-space density gradient at each unique sample.
- Gradients are linearly interpolated with the same alpha as positions and
  normalized. A zero interpolated gradient produces a zero normal.
- Material IDs are categorical unsigned 16-bit values. A surface vertex takes
  the material of the solid endpoint. Materials are not numerically blended.
- Half-resolution aliases copy all attributes from their matching unique
  full-resolution corner.

## Output and error behavior

One call writes a fixed-capacity `WtCellMesh`: at most 12 vertices and 36
8-bit indices. The backend performs no heap allocation and retains no output.
Callers supply reusable scratch storage; a thread-local scratch accessor is
provided for serial worker use.

`Empty` clears both active counts. Invalid input also clears both counts.
Non-finite scalar, origin, or gradient values return `NonFiniteInput`.
Non-positive or non-finite sizes return `InvalidScale`. Invalid transition
directions return `InvalidOrientation`. A table/input contradiction returns
`TopologyFailure`.

Triangles having exactly zero geometric area are removed. No epsilon-based
thin-triangle filtering is performed in M1; collision sanitation is an M3
decision. Partially degenerate outputs may retain vertices no longer addressed
by an index.

## Locked production defaults

- Default logical chunk size: 16 cells per axis.
- Meshing scalar window: 19 samples per axis, coordinates `-1..17` for cells
  `0..15`. The one negative and two positive layers provide cell corners and
  central-difference neighbors without cross-buffer reads.
- Runtime cell scalar: `float`; default isovalue: `0.0f`.
- Runtime material ID: categorical `uint16_t` selected from the solid edge
  endpoint.
- Runtime normal input: world-space `float3` density gradient.

Persistent scalar/material encodings remain an M4 binary-format decision.

## M1 proof obligations

`tests/native/test_wt_m1_cell_backend.cpp` runs all 256 regular cases and all
512 transition cases in every orientation (3,072 oriented transition calls).
It checks capacities, index bounds, finite positions, normal length, material
ownership, canonical orientation equivalence, representative winding,
isovalue equality, invalid input, scratch reuse, repeatability, and locked
aggregate hashes. `scripts/test_m1.ps1` requires matching debug/release hashes
and then reruns the Godot 4.6.3/4.7 addon compatibility suite.
