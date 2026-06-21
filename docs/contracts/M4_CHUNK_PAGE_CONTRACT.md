# M4 Chunk Page and Baker Contract

Status: normative M4 component

This contract defines the first authoritative `wtchunk` page and the native
deterministic baker that produces it. It builds on
`M4_STORAGE_CONTAINER_CONTRACT.md`; the common container remains responsible
for version checks, bounded offsets and sizes, codecs, SHA-256 verification,
and zero-copy section views.

## Page role

One `wtchunk` file contains the complete padded scalar and material sample
grid for one chunk key and one source revision. Pages are standalone,
content-addressable units. The `wtworld` index maps chunk keys to complete
page hashes and byte sizes without requiring a monolithic world decode.

The page stores authoritative samples, not Transvoxel case IDs, lookup-table
indices, generated vertices, render resources, collision resources, or
backend-specific data.

## Sections

The container magic is `WTCHUNK`. Feature flags are zero for schema 1.0. The
container source revision must equal the revision in `CHDR`.

Exactly two sections are required:

| FourCC | Flags | Codec | Purpose |
| --- | ---: | --- | --- |
| `CHDR` | 0 | `none` | fixed chunk metadata |
| `DATA` | 0 | `none` | interleaved scalar/material samples |

Unknown, missing, duplicate, or extra sections fail the schema reader.

## `CHDR` schema 1.0

All integers and floating-point bit patterns are little-endian. `CHDR` is
exactly 48 bytes:

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | `u16` | schema major, `1` |
| 2 | `u16` | schema minor, `0` |
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

`DATA` is exactly 41,154 bytes: 6 bytes for each of 6,859 samples.

Each record contains:

1. density as finite IEEE-754 binary32;
2. categorical material as `uint16_t`.

Samples are ordered with X changing fastest, then Y, then Z. Every axis covers
local sample coordinates `-1..17` inclusive. For chunk minimum `M` and cell
spacing `S`, sample `(x, y, z)` is evaluated at:

```text
M + (x, y, z) * S
```

This padding is the M2 meshing contract's one negative and two positive
samples around the 16-cell chunk. Non-finite densities fail baking, writing,
and decoding.

## Deterministic baker

`WtChunkBaker` accepts a construction-time page capacity, a finite list of
chunk keys, a source revision, and the native `WtChunkSampleSource`.

The baker:

- rejects a request above capacity before sampling;
- validates every key and rejects duplicates;
- sorts pages by the canonical chunk-key order `(lod, z, y, x)`;
- samples each page in the fixed Z/Y/X loop corresponding to the encoded
  order;
- clears partial output after any failure;
- writes each standalone page through the common deterministic container;
- computes a SHA-256 identity over the complete page bytes.

Input key order does not affect output order, bytes, or hashes.

## Evidence

`tests/native/test_wt_m4_bake.cpp` proves:

- unsorted and reversed inputs produce identical sorted page bytes;
- debug and release builds produce the locked aggregate page hash
  `7ed6975c20b67762bd00016b4bebd982b6aafcd4766dc3c0e6bbffaf94dfe5ce`;
- page metadata, padding coordinates, LOD spacing, sample order, source
  revision, and representative sample values round-trip exactly;
- decoded pages re-encode byte-for-byte;
- the opened `DATA` section remains a zero-copy view into page bytes;
- corruption, nonzero section flags, revision disagreement, missing or extra
  sections, capacity overflow, duplicate/invalid keys, sample-source failure,
  non-finite density, and invalid page metadata fail safely.

Typed `wtedit` transactions, invalidation, replay, compaction, migrations, and
command-line/editor entry points consume this page contract in completed M4.
