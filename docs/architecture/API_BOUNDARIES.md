# API Boundaries

The goal is not "many public APIs." A large public surface becomes another
maintenance burden. The goal is a small stable facade with multiple focused
capabilities and many internal typed services.

## Public Godot-facing classes

### `WorldTransvoxelTerrain`

High-level `Node3D` facade.

- start/stop world;
- attach configuration and data source;
- add/remove viewers;
- expose capability handles;
- emit coarse lifecycle and error signals.

It does not expose internal queues, chunk dictionaries, RIDs, or worker state.
PQ0 implements explicit asynchronous manifest startup/failure/restart/stop and
read-only lifecycle metadata. PQ1 now adds native viewer updates, global
balanced 2:1 LOD planning, and real regular/transition baked-page publication.

### `WorldTransvoxelConfig`

Versioned immutable runtime configuration resource.

- chunk sample dimensions;
- LOD count and distance policy;
- worker and frame-application budgets;
- collision policy;
- storage and cache limits;
- backend selection.

Configuration is validated once before startup.
Schema 1 is implemented as the first production-qualification foundation. It
records all M5 construction-time queue, cache, viewer, trace, application, and
collision-policy limits. Live mutation does not resize a running world;
applying another configuration requires controlled restart.

### `WorldTransvoxelDataSource`

Abstract scalar/material source.

- sample a region;
- report deterministic content revision;
- support CPU/headless sampling;
- optionally advertise compute generation.

### `WorldTransvoxelEditAPI`

Command-oriented editing.

- begin transaction;
- add typed SDF or voxel commands;
- commit/cancel;
- undo/redo where supported;
- return affected bounds and revision.

### `WorldTransvoxelQueryAPI`

Read-only world queries.

- scalar/material sample;
- surface raycast;
- loaded/collision readiness;
- chunk and LOD inspection.

### `WorldTransvoxelBakeAPI`

Editor/CLI entry point for deterministic binary baking.

### `WorldTransvoxelTelemetry`

Snapshot and event-stream access to documented metrics.

## Internal interfaces

Internal interfaces can be numerous because they are not compatibility
promises:

- meshing backend;
- sampler;
- asynchronous manifest/page store;
- edit journal;
- stream policy;
- render sink;
- collision sink;
- compression codec;
- checksum provider;
- compute service;
- trace sink.

They use C++ types rather than `Dictionary` and string-key payloads.

## API rules

- No generic `Dictionary request -> Dictionary result` hot-path APIs.
- No public method that returns mutable internal containers.
- No lookup by scene-tree group from native terrain internals.
- No gameplay manager references inside the addon.
- No backend-specific table structures in public headers.
- Every asynchronous request returns an ID or handle and completes through a
  typed event.
- Every persistent mutation has a world revision.
- Every binary or public API has an explicit version policy.
