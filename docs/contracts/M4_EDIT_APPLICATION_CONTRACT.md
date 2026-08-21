# M4 Authoritative Edit Application Contract

Status: normative M4 component

`WtChunkEditState` applies the ordered edit journal directly to decoded
authoritative chunk-page samples. It is native, deterministic, headless, and
independent of the Transvoxel meshing backend.

## Shape evaluation

Edit shape coordinates remain signed Q16 base-grid units. A page sample's
signed-64-bit world grid coordinate is promoted before multiplication by
65,536, so large valid chunk coordinates do not overflow.

Sphere inclusion is exact integer arithmetic:

```text
dx^2 + dy^2 + dz^2 <= radius^2
```

The command's validated radius is at most signed-64-bit maximum. Each axis is
first rejected when `abs(delta) > radius`; the remaining unsigned 128-bit sum
therefore fits exactly. Sphere boundaries are inclusive.

Axis-aligned boxes compare promoted Q16 coordinates against inclusive minimum
and maximum values.

The conservative integer command bounds are checked first. Commands whose
bounds do not intersect the page's padded `-1..17` sample footprint perform no
per-sample shape loop.

## Value operations

For every included sample, commands apply in journal order:

| Operation | Effect |
| --- | --- |
| add density | `density += value` |
| set density | `density = value` |
| paint material | `material = value` |
| SDF carve sphere | `density = max(density, brush_sdf)` |
| SDF construct sphere | `density = min(density, -brush_sdf)`; when the brush wins inside/on the surface and carries a nonzero material, that constructed solid takes the material |
| place static water | union the brush into the secondary static-water density; exposed terrain-air samples receive the static-water material |
| remove static water | subtract the brush from the secondary static-water density; terrain density and material remain unchanged |

Add/set/paint brushes are hard inclusive shapes. SDF sphere operations use a
one-base-grid-unit support band so the signed field remains continuous across
the brush boundary. They do not imply falloff,
blend curves, smoothing, or material interpolation. Those require explicit
future operation/schema values.

Material-aware construction is one atomic command, not a density command
followed by a paint command. It assigns the selected categorical material only
where the construct brush supplies the winning solid field. A denser
pre-existing solid and its material remain authoritative. Density-only legacy
construct commands with material zero retain their previous behavior.

Terrain edits do not delete static-water density. Construction masks water with
solid terrain. A later carve that wins at the same sample exposes the preserved
water and assigns its material to the newly empty terrain sample. This ordered
cover/reveal behavior is static volume composition, not fluid flow; the
normative details are in `STATIC_LAKE_AUTHORITY_CONTRACT.md`.

Static-water removal is the explicit inverse water operation. It applies a
signed-density difference to the water field only. It does not route through
terrain carve, and it does not restore an older terrain material in air samples.

## Terrain standard brush boundary

The current downstream terrain standard uses SDF sphere edits as the
player-facing default for digging and placing. Native M4 edit application must
therefore keep sphere evaluation deterministic, journal ordered, and independent
of rendered mesh topology.

The standard terrain edit metadata boundary is:

- operation mode;
- brush shape;
- center;
- radius or conservative bounds;
- material id when applicable;
- density value / strength;
- author and command identifiers when supplied;
- affected command bounds.

This boundary prepares later systems to inspect edited regions, but M4 does not
implement collapse, shaft stability, debris simulation, octahedron mining, or
any gameplay-specific brush. Future collapse or stability logic must operate on
voxel/SDF support data and ordered edit state, not rendered mesh triangles and
not a mandatory player brush shape.

An additive command first validates every affected result. If any result would
be non-finite, the command fails before modifying any sample or world
revision. Set-density values were already proven finite by transaction
validation.

`changed_sample_count` counts values that actually changed, not merely shape
membership.

## Revision and sequence state

Initialization requires:

- a structurally valid 19-cubed chunk page;
- finite stored densities;
- a page source revision equal to the expected baked source revision;
- an initial world edit revision.

The first command of the next transaction must use revision
`current_revision + 1` and sequence zero. Additional commands at that revision
must use the exact next sequence. Gaps, stale revisions, and sequence gaps
fail without mutation.

Commands outside a page still advance successful replay sequence and revision
state. Every page replayed through the same journal therefore reaches the same
world revision even when only some pages contain changed samples.

## Seam and persistence rule

All pages evaluate commands in absolute world coordinates. Shared padded
sample coordinates between neighboring pages must therefore produce identical
density and material values.

Edited pages retain their original baked source revision until compaction.
They can be serialized with the existing `wtchunk` writer for cache or
checkpoint use, while the ordered journal remains the authoritative edit
history.

## Evidence

`tests/native/test_wt_m4_apply.cpp` locks the debug/release edited-page hash:

```text
718299711f482dec5c606b31fefa61e6576d73f4226c0b84c5059e70031eded1
```

The test covers:

- add-density, set-density, paint-material, SDF carve, and material-aware SDF
  construct replay;
- exact sphere and box application;
- two adjacent pages and all 1,083 shared padded samples;
- changed-sample counts and revision/sequence state;
- edited page save/load/save byte agreement;
- invalid pages and source revisions;
- world revision and sequence gaps;
- invalid command bounds;
- non-finite additive results with atomic sample/revision preservation.
- static-water placement/removal order, terrain invariance, and
  construction/excavation secondary-density retention;

Snapshot compaction now turns replayed samples into a new source revision,
retains previous-world and journal audit identities, and persists the next
journal baseline. User-facing tools and final M4 evidence remain.
