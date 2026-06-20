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

These schema-1 brushes are hard inclusive shapes. They do not imply falloff,
blend curves, smoothing, or material interpolation. Those require explicit
future operation/schema values.

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
86361ec1918d415539e73091c7a9710af9bfecd21a66e3ed5f5e48a3266536df
```

The test covers:

- add-density, set-density, and paint-material replay;
- exact sphere and box application;
- two adjacent pages and all 1,083 shared padded samples;
- changed-sample counts and revision/sequence state;
- edited page save/load/save byte agreement;
- invalid pages and source revisions;
- world revision and sequence gaps;
- invalid command bounds;
- non-finite additive results with atomic sample/revision preservation.

This component does not complete M4. Compaction must turn the edited pages
into a new source snapshot, retain an audit identity linking the previous
world and journal, reset the journal baseline safely, and prove complete
bake-load-edit-save-reload equivalence.
