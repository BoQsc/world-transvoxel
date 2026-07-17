# M4 Chunk Page and Baker Contract

Status: normative M4 component

This contract defines the first authoritative `wtchunk` page and the native
deterministic baker that produces it. It builds on
`M4_STORAGE_CONTAINER_CONTRACT.md`; the common container remains responsible
for version checks, bounded offsets and sizes, codecs, SHA-256 verification,
and zero-copy section views.

## Page role

One `wtchunk` file contains the complete padded scalar, material, and
material-authoring provenance sample grid for one chunk key and one source
revision. Pages are standalone,
content-addressable units. The `wtworld` index maps chunk keys to complete
page hashes and byte sizes without requiring a monolithic world decode.

The page stores authoritative samples plus the source-derived finest-edge
records required by standard Transvoxel surface shifting. It does not store
Transvoxel case IDs, lookup-table indices, generated vertices, render
resources, collision resources, or backend-specific data.

## Sections

The container magic is `WTCHUNK`. Feature flags are zero for schema 1.2. The
container source revision must equal the revision in `CHDR`.

Schema 1.2 requires exactly three sections:

| FourCC | Flags | Codec | Purpose |
| --- | ---: | --- | --- |
| `CHDR` | 0 | `none` | fixed chunk metadata |
| `DATA` | 0 | `none` | interleaved scalar/material samples |
| `SHFT` | 0 | `none` | sparse finest-resolution edge constraints |

Unknown, missing, duplicate, or extra sections fail the schema reader.
Legacy schemas 1.0 and 1.1 remain readable. Schema 1.0 has exactly `CHDR` and
`DATA`; because it contains no `SHFT` data, a decoded 1.0 page above LOD0 is not
eligible for multiresolution meshing until its surface-shift records are
regenerated. Schema 1.1 has all three sections but no material-authoring
provenance. Readers conservatively initialize missing provenance to `true` so
an unknown legacy material is rendered categorically and is never silently
overridden by a procedural presentation. Recovering base-source provenance
requires rebaking the base and replaying its edits into schema 1.2.

## `CHDR` schema 1.2

All integers and floating-point bit patterns are little-endian. `CHDR` is
exactly 48 bytes:

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | `u16` | schema major, `1` |
| 2 | `u16` | schema minor, `2` |
| 4 | `i32` | chunk X |
| 8 | `i32` | chunk Y |
| 12 | `i32` | chunk Z |
| 16 | `u8` | LOD, `0..20` |
| 17 | `u8` | minimum sample coordinate, two's-complement `-1` (`255`) |
| 18 | `u8` | maximum sample coordinate, `17` |
| 19 | `u8` | reserved, zero |
| 20 | `u16` | X sample dimension, `19` |
| 22 | `u16` | Y sample dimension, `19` |
| 24 | `u16` | Z sample dimension, `19` |
| 26 | `u8` | density encoding, IEEE-754 binary32 (`1`) |
| 27 | `u8` | material encoding, unsigned 16-bit (`1`) |
| 28 | `u32` | sample count, `6859` |
| 32 | `u64` | cell spacing, exactly `1 << lod` |
| 40 | `u64` | authoritative source revision |

Readers reject unsupported schema versions, nonzero reserved data, invalid
chunk keys, dimensions or padding other than the fixed 19-cubed contract,
unknown encodings, an incorrect sample count or spacing, and revision
disagreement with the common container.

## `DATA` encoding

`DATA` is exactly 48,013 bytes: 7 bytes for each of 6,859 samples.

Each record contains:

1. density as finite IEEE-754 binary32;
2. categorical material as `uint16_t`;
3. material-authoring provenance as `uint8_t`, exactly `0` or `1`.

Samples are ordered with X changing fastest, then Y, then Z. Every axis covers
local sample coordinates `-1..17` inclusive. For chunk minimum `M` and cell
spacing `S`, sample `(x, y, z)` is evaluated at:

```text
M + (x, y, z) * S
```

This padding is the M2 meshing contract's one negative and two positive
samples around the 16-cell chunk. Non-finite densities fail baking, writing,
and decoding.

Regular gradients use this page's declared spacing. Higher-LOD transition
faces obtain their half-spacing samples and gradients from the four canonical
LOD-minus-one support pages defined by
`M5_TRANSITION_PAGE_SOURCE_CONTRACT.md`.

## `SHFT` encoding and invariant

`SHFT` is sparse. Its 8-byte header contains the finite `float32` isovalue and
a `u32` record count. Each following 44-byte record contains:

1. the `u16` canonical regular-grid edge index;
2. the `u32` offset of the selected unit edge from the coarse edge's lower
   endpoint;
3. density, XYZ gradient, material, and material-authoring provenance for both
   unit-edge endpoints.

Records are strictly ordered by edge index and exist exactly for regular
coarse edges whose coarse endpoints straddle the stored isovalue. An LOD0
page has no records because its regular edges are already unit edges. For
LOD `L > 0`, baking recursively bisects each sign-changing length-`2^L` edge
using authoritative unit-resolution samples until it selects the sign-changing
unit edge. Meshing interpolates the coarse vertex from those unit endpoints,
so the coarse and finest meshes use the same surface position as required by
Lengyel's surface-shifting construction.

The format rejects unordered, duplicate, out-of-range, non-finite, same-sign,
wrong-isovalue, or otherwise inconsistent records. A missing or invalid record
is a meshing failure; the runtime must not silently fall back to interpolation
on the coarse endpoints.

## Deterministic baker

`WtChunkBaker` accepts a construction-time page capacity, a finite list of
chunk keys, a source revision, and the native `WtChunkSampleSource`.

The baker:

- rejects a request above capacity before sampling;
- validates every key and rejects duplicates;
- sorts pages by the canonical chunk-key order `(lod, z, y, x)`;
- samples each page in the fixed Z/Y/X loop corresponding to the encoded
  order;
- resolves every sign-changing higher-LOD regular edge to its authoritative
  unit edge and writes the deterministic sparse `SHFT` section;
- clears partial output after any failure;
- writes each standalone page through the common deterministic container;
- computes a SHA-256 identity over the complete page bytes.

Input key order does not affect output order, bytes, or hashes.

## Evidence

`tests/native/test_wt_m4_bake.cpp` proves:

- unsorted and reversed inputs produce identical sorted page bytes;
- debug and release builds produce the locked aggregate page hash
  `85b71cb2803a1d7c405f20bc287c17c9384487e63f1ed6b61d5c179111756fb3`;
- page metadata, padding coordinates, LOD spacing, sample order, source
  revision, representative sample values, and material-authoring provenance
  round-trip exactly;
- decoded pages re-encode byte-for-byte;
- the opened `DATA` section remains a zero-copy view into page bytes;
- corruption, nonzero section flags, revision disagreement, missing or extra
  sections, capacity overflow, duplicate/invalid keys, sample-source failure,
  non-finite density, and invalid page metadata fail safely.

Typed `wtedit` transactions, invalidation, replay, compaction, migrations, and
command-line/editor entry points consume this page contract in completed M4.
