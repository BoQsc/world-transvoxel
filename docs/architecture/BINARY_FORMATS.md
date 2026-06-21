# Binary Format Direction

Binary formats are part of the architecture, not an optimization added after
JSON and runtime objects become too slow.

## Format family

```text
*.wtworld   immutable baked world bundle and section directory
*.wtchunk   independently cacheable chunk page
*.wtedit    append-only edit command journal
*.wttrace   optional binary telemetry trace
```

## Common header

All formats begin with:

```text
magic[8]
format_major:u16
format_minor:u16
header_size:u32
feature_flags:u64
source_revision:u64
directory_offset:u64
directory_size:u64
payload_hash[32]
```

Rules:

- little-endian on disk;
- fixed-width integers;
- explicit bounds and section sizes;
- unknown optional sections are skipped;
- unknown required feature flags fail cleanly;
- no serialized Godot `Variant` or `store_var` payloads;
- no native pointer-sized or compiler-dependent structs;
- readers validate all offsets before allocation;
- content hashes are independent of filesystem timestamps.

## World sections

Candidate sections:

```text
META    world dimensions, coordinate system, generator identity
LOD0..N baked scalar/material pages or source descriptors
BIOM    material/biome map
WATR    water or secondary-field data
EDIT    compact initial edit journal
INDX    spatial page directory
DEPS    source and toolchain manifest
```

Sections are independently compressible. Compression is a codec field, not an
implicit file mode. The initial implementation must support `none`; an
additional codec is added only with deterministic cross-platform tests.

## Chunk pages

Chunk records are designed for direct range reads:

```text
chunk_key
lod
sample_format
material_format
uncompressed_size
compressed_size
content_revision
content_hash
payload
```

Meshes are caches, not authoritative world data. The authoritative inputs are
baked samples/source descriptors plus ordered edits.

The M5 filesystem reader resolves pages by full-page SHA-256 filename and
validates size, hash, schema, chunk key, and source revision before publishing
an immutable asynchronous completion.

## Edit journal

Each edit record contains:

```text
record_size
schema_version
command_id
world_revision
author_id
operation
shape
flags
material
bounds
typed payload
record_checksum
```

Transactions commit atomically through a journal footer or commit record.
Compaction produces a new snapshot and retains a migration audit.

## Bake reproducibility

Every baked artifact records:

- source asset hashes;
- generator version;
- configuration hash;
- upstream Transvoxel revision;
- Godot/godot-cpp/toolchain revisions;
- format version;
- deterministic output hash.
