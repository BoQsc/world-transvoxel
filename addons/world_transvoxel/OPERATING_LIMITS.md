# World Transvoxel 1.0.13-dev Operating Limits

## Qualified release matrix

The 1.0.13-dev development build inherits the 1.0.9 Windows x86-64
qualification matrix, includes the documented 1.0.10-dev batched authoritative
sample query, keeps native render transition fading opt-in/default-off, keeps
fade shader instance-parameter writes opt-in/default-off, and adds the
quantized mesh-finalizer topology boundary documented below. It is
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

## Mesh finalizer and topology boundary

- The finalizer builds its edge-connectivity graph from quantized position keys
  at 1/1024 world-unit precision. This is a connectivity/orientation key only;
  it does not snap exported vertex positions.
- The official M2 mesh topology hash remains `20a67f299820f5c3`.
- Matched interior or chunk-face near-zero connector slivers may exist after
  official Transvoxel meshing and dynamic edits. They are accepted only when the
  topology probe proves they are not open holes: `boundary_edges=0`,
  `interior_boundary_edges=0`, `unknown_boundary_edges=0`,
  `nonmanifold_edges=0`, `orientation_conflict_edges=0`,
  `repeated_point_key_triangles=0`, `zero_area_unknown_triangles=0`, and
  `zero_edge_triangles=0`.
- Unknown zero-area triangles, repeated-point-key triangles, zero-edge
  triangles, interior/unknown boundary edges, nonmanifold edges, and orientation
  conflicts are hard failures.
- Do not delete matched connector slivers as a generic cleanup step. That was
  tested and opened real cracks in edited terrain. If a downstream profile sees
  sky through terrain, it must be diagnosed as topology, culling/material, or
  streaming/LOD continuity, not hidden by deleting triangles blindly.
- Double-sided terrain, duplicate backstop geometry, and full-map/backdrop
  presentation layers are not part of this topology claim. They may hide visual
  symptoms but do not prove mesh correctness.

## Runtime configuration

| Property | Default | Maximum |
| --- | ---: | ---: |
| active chunks | 256 | 65,536 |
| viewers | 8 | 1,024 |
| demands per viewer | 4,096 | 65,536 |
| LOD refinement radius cap | 0, use viewer radius | 65,536 |
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
65,536. `lod_refinement_radius_chunks=0` means no cap; nonzero values must not
exceed demand capacity per viewer. Deactivation distance must be at least
activation distance.

## Streaming and edited-LOD continuity

- Moving-viewer terrain is streamed from an active desired chunk set. Projects
  should expect coarse far terrain plus detailed chunks around active viewers,
  not every LOD0 chunk of a large world resident at once.
- Edited terrain LOD fidelity is a budgeted runtime guarantee, not an unlimited
  world-state guarantee. A project may dig or place more terrain, over a wider
  area, than the active chunk capacity and edit-retention planner budget can keep
  at near detail from a distant camera. In that case older, farther, or lower
  priority edited regions may be represented by coarser LOD until a viewer moves
  close enough to refine them again.
- Runtime mesh watertightness and streaming visual continuity are separate
  claims. A mesh probe with zero interior boundary/nonmanifold edges does not by
  itself prove that every possible camera path is free from transient loading or
  LOD-popping artifacts.
- Recent edit LOD-retention zones are promoted into temporary planner viewers so
  recently dug or placed terrain remains detailed longer when the player moves
  away and returns. The current implementation keeps the newest eight
  edit-retention zones active even without a real viewer nearby, then still
  applies the normal retention-zone merge and capacity limits. When the full
  retention plan exceeds capacity, runtime planning degrades retention by keeping
  the newest/visible zones first and reducing retention refinement before dropping
  retention entirely. This prevents the known all-or-nothing fallback failure, but
  it does not make every far edit permanently high-detail.
- Any downstream game or addon that depends on long-distance visibility of mined,
  dug, placed, or restored terrain must explicitly budget and validate it. The
  required validation class is: create player-like edits, move/fly close, mid,
  far, and back, then assert edit persistence, no open/nonmanifold rendered
  geometry, and acceptable far-LOD shape continuity for that game's camera
  distances and performance settings.
- Render transition fading is opt-in and disabled by default. Enabling
  `render_transition_frames` is a presentation choice; it must not be used as a
  substitute for missing geometry or as proof that terrain is seamless.
- Projects that need aggressive high-speed flight must qualify the chosen
  viewer radius, active chunk capacity, viewer update distance, render/collision
  apply budgets, and storage/meshing throughput together. Raising one value in
  isolation can increase churn instead of improving visible continuity.

## Geometry and world bounds

- Chunk cells per axis: 16.
- Stored samples per axis: 19, including fixed negative/positive padding.
- Maximum LOD: 20.
- Regular chunk: 49,152 vertices and 61,440 indices maximum.
- Each transition face: 3,072 vertices and 9,216 indices maximum.
- One chunk may own up to six transition faces.
- World manifest: 262,144 pages maximum.
- Compact procedural world descriptor: 262,144 indexed hierarchy pages maximum.
  The current deterministic source emits LOD0..LOD3 pages with eight LOD0,
  four LOD1, two LOD2, and one LOD3 vertical layers for the configured chunk
  slice.
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
