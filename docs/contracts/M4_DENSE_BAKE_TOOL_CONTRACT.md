# M4 Dense Volume Bake Tool Contract

Status: normative M4 component

The command-line baker accepts a finite dense scalar/material volume and uses
the native `WtChunkBaker` to produce standalone content-addressed pages and a
world manifest.

```console
python tools/wt_bake.py density.f32 keys.txt output-directory \
  --materials materials.u16 \
  --origin -1 -1 -1 \
  --dimensions 35 19 19 \
  --spacing 1 \
  --source-revision 7
```

Python owns argument validation, canonical configuration identity, provenance,
and native process launch. Density/material loading, source lookup, chunk
sampling, page writing, hashing, and world-manifest generation are native.

## Dense input

Density input is raw little-endian IEEE-754 binary32. Material input, when
present, is raw little-endian `uint16_t`. Both use X-fastest, then Y, then Z
order and contain exactly:

```text
dimension_x * dimension_y * dimension_z
```

records.

The grid descriptor contains:

- signed-64-bit integer world origin;
- positive 32-bit dimensions;
- positive integer sample spacing;
- default `uint16_t` material when no material file is supplied.

Requested sample points must be inside the volume and exactly aligned to its
spacing. Non-finite density, malformed byte counts, overflow, unaligned
samples, and uncovered chunk padding fail.

## Chunk key input

The key file is UTF-8/ASCII whitespace-separated text with one:

```text
x y z lod
```

record per chunk. Coordinates must fit signed 32-bit and LOD must satisfy the
native chunk contract. Empty, malformed, invalid, duplicate, or over-capacity
lists fail through the native reader/baker.

Input order does not affect output bytes.

## Output

The requested output directory must not exist. Baking occurs in one sibling
temporary directory:

```text
world.wtworld
<complete-page-sha256>.wtchunk
<complete-page-sha256>.wtchunk
...
```

The temporary directory is renamed only after every page and the manifest
close successfully. Failure removes only that tool-owned temporary directory.
Existing output is never overwritten.

The world manifest records:

- complete density and material/default-material source hashes;
- canonical bake configuration hash;
- addon/generator version;
- pinned official MIT backend hash and revision;
- supported Godot matrix;
- godot-cpp revision;
- Zig toolchain version;
- source revision and page identities.

The configuration hash is SHA-256 of canonical JSON containing input hashes,
origin, dimensions, spacing, key-file hash, source revision, and default
material.

## Evidence

`scripts/test_m4_bake_tool.py` builds an independent 35x19x19 dense volume and
proves:

- two requested chunks bake successfully;
- debug and release output directories are byte-identical;
- the world and page artifacts validate through the native storage tool;
- decoded center density/material values match source coordinates;
- default-material baking works without a material volume;
- non-finite density fails without partial output;
- uncovered padded sampling fails without partial output.

No generated bake artifacts remain in the repository.

This is the M4 command-line bake entry point. The editor plugin may launch the
same Python/native path as non-critical scaffolding; it must not reimplement
sampling or serialization in GDScript.
