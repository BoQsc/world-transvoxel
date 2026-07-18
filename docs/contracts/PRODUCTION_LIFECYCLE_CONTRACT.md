# Production World Lifecycle Contract

Status: complete for production qualification PQ0

## Scope

This contract connects `WorldTransvoxelTerrain` to a validated `wtworld`
manifest and the existing bounded asynchronous storage worker. It owns startup
and shutdown. The separate `PRODUCTION_READ_ONLY_STREAMING_CONTRACT.md` now
uses that running ownership for the first PQ1 pipeline.

The lifecycle does not use the M3 or M5 test fixtures. `WorldTransvoxelTerrain`
owns a focused native `WtWorldLifecycleService`, which in turn owns the M5
storage service for exactly one world generation.

## State machine

The only valid state sequence is:

```text
stopped -> starting -> running -> stopping -> stopped
                    \
                     -> failed -> stopping -> stopped
```

Stable numeric values exposed by `get_world_state()` are:

```text
0 stopped
1 starting
2 running
3 stopping
4 failed
```

Starting from any state other than `stopped` fails without replacing the
active runtime. A failed lifecycle must pass through asynchronous stop before
restart. A stopped lifecycle may start again with a new immutable
configuration snapshot.

## Thread and ownership boundary

`start_world(manifest_path, object_root)` validates that an explicit schema-1
configuration is attached, copies it, globalizes Godot `res://` or `user://`
paths, creates bounded storage ownership, and returns after launching a native
control thread.

`start_procedural_world(chunk_count_x, chunk_count_z, seed, source_revision,
object_root)` follows the same lifecycle and bounded storage ownership, but
starts from a compact deterministic descriptor instead of a manifest. Requested
pages are generated on demand through the existing native page baker and then
flow through the same cache, meshing, editing, and application pipeline as
manifest-backed pages.

`start_flat_world(chunk_count_x, chunk_count_z, source_revision, object_root)`
follows the same lifecycle and bounded storage ownership with the procedural
descriptor set to flat mode. It produces a flat y=8 surface while retaining the
same page format, cache, meshing, editing, collision, and LOD streaming path.

The named `rolling_hills_cave_roads` preset exercises a volumetric
infrastructure source through that same lifecycle. Its road network is a
continuous world-space field: centerline grades modify density through smooth
shoulders, and a shallow material-ID `10` layer identifies asphalt. This keeps
the reference road independent of chunk boundaries and avoids encoding a
network as thousands of unrelated sphere edits.

The named `four_biomes_lakes_caves_roads` preset exercises a larger authored
world composition through the same lifecycle. Four categorical biome regions,
three lake volumes, three compact caves, mountains, rolling detail, and an
18-segment connected road graph all remain deterministic world-space source
fields. Each lake owns an explicit secondary signed-density field whose zero
crossing is its gravity-aligned free surface and whose horizontal domain bounds
the intended catchment. The water field is meshed independently of terrain LOD;
the terrain surface depth-occludes portions beneath solid ground, while material
ID `9` labels occupied terrain-air samples for queries and presentation. This
separation prevents categorical material sampling or a terrain-LOD boolean
intersection from moving the lake surface or shoreline. Material ID `10`
remains a shallow solid asphalt layer. These fields are independent of chunk
decomposition and viewer LOD.

The control thread:

1. reads and validates the manifest;
2. starts the existing sleeping storage worker;
3. publishes source revision, world revision, and page count;
4. waits without polling for a stop request;
5. closes the storage worker and clears manifest metadata;
6. publishes `stopped`.

Manifest I/O and storage-worker join never occur in `start_world()` or
`stop_world()` on the Godot frame thread. `stop_world()` only records a request
and returns. Final object destruction joins owned native threads so no worker
outlives the terrain node.

The configuration resource cannot be replaced while state is starting,
running, failed, or stopping. Mutating the Resource object does not change the
copied native startup configuration.

## Godot API

The facade exposes:

- `start_world(world_manifest_path, object_root) -> bool`;
- `start_procedural_world(chunk_count_x, chunk_count_z, seed, source_revision, object_root) -> bool`;
- `start_flat_world(chunk_count_x, chunk_count_z, source_revision, object_root) -> bool`;
- `stop_world() -> bool`;
- state number/name and `is_world_running()`;
- source revision, world revision, and manifest page count;
- deterministic synchronous or asynchronous error text;
- `world_state_changed(state, state_name)`;
- `world_failed(error)`.

Missing/corrupt manifests are accepted as asynchronous startup attempts and
transition to `failed`; they are not misreported as synchronous argument
errors. Empty API paths and invalid configuration fail synchronously before a
thread is created.

Procedural and flat startup reject invalid dimensions, missing object roots,
nonpositive source revisions, and descriptors above the 262,144 hierarchy-page
compact runtime limit synchronously before a thread is created.

## Proof

`test_wt_production_lifecycle.cpp` proves valid startup, manifest metadata,
double-start rejection, asynchronous stop, restart, missing/corrupt manifest
failure, invalid configuration, immediate start/stop, and destructor cleanup.
Debug and release hashes must match:

```text
ccdb1e1ad000f824ebd4628e640a6c1d95f9d734cc1298f738de3d0c98f3a126
```

`production_lifecycle_test.gd` proves the public API, signals, path
globalization, immutable running configuration, asynchronous failure/reset,
restart, and queue-free cleanup on Godot 4.6.3 and 4.7 with both addon builds.

`test_wt_m5_async_storage.cpp` proves procedural descriptor validation,
procedural page indexing, synchronous and asynchronous generated page loads,
manifest-snapshot rejection in procedural mode, metrics accounting, and clean
close behavior.

The production qualification entry point generates its deterministic manifest
fixture through the native format writer before running Godot. No binary
fixture is hand-maintained or decoded in GDScript.

PQ0 is complete when this contract and the configuration contract pass
together. PQ1 regular/transition streaming and the root example use this same
lifecycle and now pass its exit audit.
