# M4 Edit Spatial Invalidation Contract

Status: normative M4 component

The edit spatial index maps authoritative edit bounds to the exact set of
indexed chunk pages whose padded scalar/material samples may have changed.
It is an in-memory native index; it does not add another persistent format.

## Indexed universe

The index is rebuilt from a bounded, unique collection of valid chunk keys.
For a baked world these are the keys in the `wtworld` page index. Runtime
streaming may build the same index over another explicitly owned key set.

Rebuild is atomic: invalid input, duplicate keys, or capacity overflow leaves
the previous index unchanged.

Keys are stored in a spatial hash. Queries enumerate only the mathematically
affected coordinate range at populated LOD levels and perform direct hash
lookups. They do not scan all indexed chunks.

## Padded dependency rule

A chunk at LOD `L` has:

```text
cell spacing = 1 << L
cell extent  = 16 * cell spacing
```

Its authoritative page samples local coordinates `-1..17`, so its inclusive
sample footprint on each axis is:

```text
chunk minimum - cell spacing
through
chunk maximum + cell spacing
```

A chunk is invalidated when this padded footprint intersects an edit's
conservative integer bounds on all three axes.

This rule deliberately includes:

- the directly edited chunk at each indexed LOD;
- adjacent same-LOD chunks that consume changed border or gradient samples;
- every indexed coarser-LOD page whose samples depend on the edited region;
- coarse transition owners whose page samples may have changed.

Testing only the unpadded 16-cell chunk bounds is incorrect because it misses
seam and central-difference consumers.

## Bounded behavior

`WtEditSpatialIndex` has three construction-time limits:

| Limit | Meaning |
| --- | --- |
| key capacity | maximum indexed chunk keys |
| candidate capacity | maximum coordinate candidates checked by one query |
| result capacity | maximum unique invalidated keys returned |

Each transaction command is queried separately and results are unioned. This
avoids invalidating the unrelated volume between distant commands.
Candidate use is accumulated across the transaction, and duplicate results do
not consume result capacity twice.

Queries fail and clear output on:

- invalid or inverted bounds;
- invalid transaction identity, revisions, sequences, command IDs, or shapes;
- candidate capacity overflow;
- result capacity overflow.

Coordinate arithmetic is signed-64-bit and saturates before division, so
extreme caller-provided bounds fail by capacity rather than overflowing.

Successful results are unique and sorted by canonical chunk order
`(lod, z, y, x)`.

## Evidence

`tests/native/test_wt_m4_spatial.cpp` locks the debug/release evidence hash:

```text
dd58c70452ae48e8e32d582d769c13ccfe235b64d612aba668e4ad15d89ef513
```

The test covers:

- 24 padded origin dependents across LOD 0, 1, and 2;
- interior edits that affect only one page per indexed LOD;
- negative exact-boundary edits and adjacent sample consumers;
- separated transaction commands without bridge-volume invalidation;
- canonical deterministic output;
- atomic rebuild failure;
- invalid and duplicate keys;
- inverted and extreme bounds;
- candidate and result capacity limits;
- invalid transaction sequences.

Journal ordering, authoritative application, compaction, migration, and tools
consume the same transaction bounds in the completed M4 suite.
