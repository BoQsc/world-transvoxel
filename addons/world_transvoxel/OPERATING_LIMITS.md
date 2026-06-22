# World Transvoxel 1.0.1 Operating Limits

## Qualified release matrix

The 1.0.1 corrective release is qualified only for:

| Component | Supported value |
| --- | --- |
| Operating system | Windows 10/11 x86-64 |
| Godot | 4.6.3 and 4.7 |
| Runtime builds | `template_debug` and `template_release` |
| Renderer used by headless qualification | Compatibility |
| Native toolchain used to build release | Zig 0.16.0 |
| godot-cpp revision | `e83fd0904c13356ed1d4c3d09f8bb9132bdc6b77` |
| Terrain backend | official Transvoxel MIT backend |

Other platforms, architectures, and Godot versions are not qualified by this
release even if the source can be compiled for them.

## Runtime configuration

| Property | Default | Maximum |
| --- | ---: | ---: |
| active chunks | 256 | 65,536 |
| viewers | 8 | 1,024 |
| demands per viewer | 4,096 | 65,536 |
| total configured demands | derived | 65,536 |
| storage requests/completions | 256 each | 65,536 each |
| encoded page cache | 256 / 64 MiB | 65,536 / 1 GiB |
| decoded page cache | 128 / 64 MiB | 65,536 / 1 GiB |
| mesh cache | 128 / 128 MiB | 65,536 / 1 GiB |
| render cache | 128 / 128 MiB | 65,536 / 1 GiB |
| collision cache | 64 / 64 MiB | 65,536 / 1 GiB |
| trace events | 65,536 | 262,144 |
| render apply budget per frame | 4 | 128 |
| collision apply budget per frame | 2 | 128 |
| collision activation/deactivation | 96 / 128 | finite, nonnegative |

Viewer capacity multiplied by demand capacity per viewer may not exceed
65,536. Deactivation distance must be at least activation distance.

## Geometry and world bounds

- Chunk cells per axis: 16.
- Stored samples per axis: 19, including fixed negative/positive padding.
- Maximum LOD: 20.
- Regular chunk: 49,152 vertices and 61,440 indices maximum.
- Each transition face: 3,072 vertices and 9,216 indices maximum.
- One chunk may own up to six transition faces.
- World manifest: 262,144 pages maximum.
- Manifest dependency records: 1,024 maximum; dependency text is 255 bytes.

## Storage, editing, and operations

- Common container size: 256 MiB maximum.
- Container section: 64 MiB maximum; 4,096 sections maximum.
- Production edit journal: 4,096 transactions, 65,536 commands, and 64 MiB.
- One edit transaction: 4,096 commands maximum.
- Runtime world-operation queue: 16 requests.
- Side-by-side snapshot compaction: 4,096 pages and 256 MiB of source page
  bytes maximum.
- Storage CLI input: 1 GiB maximum, while common containers retain the
  stricter 256 MiB limit.
- Schema-1 storage codec is `none`; compressed storage is not supported.

Compaction and migration never overwrite the live world. The output directory
must not exist, is published atomically after completion, and must be reopened
through a controlled stop/start before it becomes active.

## Operational requirements

- Runtime access is event-driven. Applications must send viewer updates when
  position or demand changes; there is no implicit scene viewer discovery.
- Keep viewer revisions monotonic and remove viewers when no longer active.
- Polling readiness is allowed, but signals and event-driven application code
  are preferred.
- Authoritative sample queries can fail for absent, corrupt, misaligned, or
  disagreeing overlapping pages.
- Output paths for bake, migration, and compaction must not already exist.
- Python 3.11 or newer is required only for packaged command-line/editor tools,
  not for runtime terrain use.
