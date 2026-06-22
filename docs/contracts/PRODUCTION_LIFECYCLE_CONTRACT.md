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

The production qualification entry point generates its deterministic manifest
fixture through the native format writer before running Godot. No binary
fixture is hand-maintained or decoded in GDScript.

PQ0 is complete when this contract and the configuration contract pass
together. PQ1 regular and transition streaming use this same lifecycle; its
remaining gate is the root example and exit audit.
