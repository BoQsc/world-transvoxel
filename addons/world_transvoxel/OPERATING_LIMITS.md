# World Transvoxel 1.0.12-dev Operating Limits

## Qualified release matrix

The 1.0.12-dev S2 development build inherits the 1.0.9 Windows x86-64
qualification matrix, includes the documented 1.0.10-dev batched authoritative
sample query, makes native render transition fading opt-in/default-off, and
makes fade shader instance-parameter writes opt-in/default-off so stable
large-scale scenes do not blink during edits or exhaust Godot instance-shader
storage. It is
qualified only for:

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
| ready chunk retirement removals per frame | 4 | fixed in 1.0.4 |
| render transition fade duration | 0 frames, disabled | 240 |
| same-key render mesh replacement | direct swap by default | crossfade when transition frames > 0 |
| shader fade opacity parameter | `wt_fade_opacity`, opt-in/default-off | fixed in 1.0.11-dev |
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
- Compact procedural world descriptor: 262,144 indexed LOD-0 pages maximum,
  currently one vertical chunk layer.
- Manifest dependency records: 1,024 maximum; dependency text is 255 bytes.

## Storage, editing, and operations

- Common container size: 256 MiB maximum.
- Container section: 64 MiB maximum; 4,096 sections maximum.
- Production edit journal: 4,096 transactions, 65,536 commands, and 64 MiB.
- One edit transaction: 4,096 commands maximum.
- Runtime world-operation queue: 16 requests.
- One authoritative sample batch query: 4,096 grid points maximum.
- Side-by-side snapshot compaction: 4,096 pages and 256 MiB of source page
  bytes maximum.
- Storage CLI input: 1 GiB maximum, while common containers retain the
  stricter 256 MiB limit.
- Dense bake input is file-backed: finite-density validation uses a fixed
  1 MiB buffer, source sampling uses a 192 KiB explicit block cache, and one
  encoded page payload is retained at a time.
- Bake key and manifest metadata still scale with requested pages and remain
  bounded by the 262,144-page world-manifest limit.
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
- Moving-viewer plan changes retain retiring render/collision chunks until the
  current replacement set is fully ready. During sustained movement, resource
  and application-record ownership can temporarily approach twice the active
  chunk capacity; it returns to the current desired set after streaming settles.
  Once replacements are fully ready, old chunk removal is capped at four chunks
  per frame to avoid a large one-frame dynamic LOD visual swap. Render
  transition fading is disabled by default: same-key render mesh replacements
  swap directly, so terrain edits do not create a white blink/fade. Projects
  that explicitly set `WorldTransvoxelConfig.render_transition_frames` above
  zero opt into temporary retiring render instances, retirement fade-out, and
  introduction fade-in for that many frames. Custom terrain shaders that want
  deterministic native fade behavior through `ALPHA` must declare an instance
  uniform named `wt_fade_opacity` with default `1.0`, apply it to `ALPHA`, set a
  positive render transition frame count, and explicitly enable
  `WorldTransvoxelConfig.shader_fade_parameter_enabled`. This switch is off by
  default because Godot retains per-instance shader-parameter slots after use;
  stable large-scale scenes must keep the default unless the project has a
  measured shader-slot budget. Collision is removed at retirement.
- Authoritative sample queries can fail for absent, corrupt, misaligned, or
  disagreeing overlapping pages.
- Output paths for bake, migration, and compaction must not already exist.
- Python 3.11 or newer is required only for packaged command-line/editor tools,
  not for runtime terrain use.
