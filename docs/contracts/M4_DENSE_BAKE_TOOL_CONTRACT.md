# M4 Dense Volume Bake Tool Contract

Status: normative M4 component

The command-line baker accepts a finite dense scalar/material volume and uses
the native `WtChunkBaker` to produce standalone content-addressed pages and a
world manifest. Dense describes the input layout, not an unbounded allocation:
the production implementation is file-backed and page-bounded.

```console
python addons/world_transvoxel/tools/wt_bake.py density.f32 keys.txt output-directory \
  --materials materials.u16 \
  --origin -1 -1 -1 \
  --dimensions 35 19 19 \
  --spacing 1 \
  --source-revision 7
```

The root `tools/wt_bake.py` remains a source-checkout compatibility shim.
Python owns argument validation, streaming source hashing, canonical
configuration identity, provenance, and native process launch. Native code
owns source-size and finite-density validation, bounded file-backed source
lookup, chunk sampling, page writing, and world-manifest generation.

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

## Bounded lifetime

The native tool must not retain complete source arrays or all encoded pages.

- density validation uses a fixed 1 MiB streaming buffer;
- source lookup uses a fixed 512-slot cache of 64-sample density/material
  blocks: 192 KiB of explicit sample payload;
- exactly one encoded page payload is live during page output;
- completed page bytes are written immediately into the temporary directory;
- only the bounded key list and compact manifest page index grow with page
  count, up to the schema limit of 262,144 pages.

This replaces the original raw-source plus decoded-source plus all-page
lifetime without changing dense input bytes, page bytes, world schema, source
hashes, configuration identity, or deterministic key ordering.

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
- the tool reports bounded operation, one peak page payload, and a 192 KiB
  explicit source cache;
- the world and page artifacts validate through the native storage tool;
- decoded center density/material values match source coordinates;
- default-material baking works without a material volume;
- non-finite density outside requested chunks still fails before output;
- a later uncovered chunk fails and removes already-written temporary pages.

The bounded release fixture page files were compared byte-for-byte with the
installed World Transvoxel 1.0.2 release baker. The page files matched. The
world manifest includes release-versioned generator provenance, so the locked
World Transvoxel 1.0.5 world SHA-256 is:

```text
8fb72856edd71088115bb77c7761cad7c3155eafed25656973f97bed732dc252
```

No generated bake artifacts remain in the repository.

This is the M4 command-line bake entry point. The editor plugin launches the
same Python/native path as non-critical scaffolding and does not reimplement
sampling or serialization in GDScript.
