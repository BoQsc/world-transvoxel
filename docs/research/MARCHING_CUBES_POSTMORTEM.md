# Marching Cubes Architecture Postmortem

Audit source:

```text
C:\Users\Windows10_new\Documents\gpu-marching-cubes
```

Audit date: 2026-06-20.

This is not a criticism of the features achieved by that project. It records
structural failure modes that World Transvoxel must prevent from the first
commit.

## Measured concentration

`world_marching_cubes/chunk_manager.gd` currently contains:

```text
9,116 lines
356 functions
109 exported settings
331 top-level variables/constants
4 signals
```

The file owns or coordinates terrain generation, water, materials, world-map
loading, LOD, visual batching, shadows, rendering resources, GPU queues, CPU
queues, collision bodies, edits, roads, vegetation notifications, building
lookups, prefab lookups, spawning, runtime power policy, telemetry, and scene
node lifecycle.

The accompanying architecture document says the same file has 2,240 lines and
111 functions. Documentation no longer describes the implementation.

Other concentrated files include:

```text
gdextension/src/mesh_builder.cpp                 3,229 lines
world_map_generator/world_map_generator.gd       3,009 lines
world_building_system/building_manager.gd        2,985 lines
world_building_system/prefab_spawner.gd          2,309 lines
gdextension/src/prefab_geometry_native.cpp       1,796 lines
```

`mesh_builder.cpp` mixes terrain mesh conversion, collision construction,
material textures, building meshes, baked-building payloads, world-map LOD
meshes, merged visual batches, and collision boxes. Moving code to C++ did not
by itself create a subsystem boundary.

## What worked and should be retained

- Native C++ avoids expensive high-level mesh construction paths.
- Packed byte arrays and compact material storage are directionally correct.
- Compute shaders are useful for bulk density generation.
- Collision distance and per-frame application budgets are necessary.
- Telemetry exposed real bottlenecks.
- Baking and pregeneration reduce runtime work.
- Persistent edit commands are preferable to storing complete regenerated
  density volumes.
- The project accumulated real runtime evidence, not only isolated unit tests.

## Failure modes to prevent

### One manager became every subsystem

Feature additions had no mandatory ownership boundary. State, policy,
scheduling, GPU resources, scene nodes, storage, and gameplay integration
accumulated in one script.

Rule: the public terrain node delegates to services. It does not own their
queues, caches, algorithms, and serialization details.

### Polling replaced events

Large `_process` flows repeatedly inspect global state, queues, chunk sets,
FPS, external managers, visual batches, collisions, and power state.

Rule: chunk state changes produce typed events. Schedulers consume dirty sets
and completion queues. No subsystem scans all chunks merely to discover work.

### Dynamic dictionaries crossed hot boundaries

Many tasks and results use `Dictionary`, string keys, and `Variant` payloads.
This makes contracts implicit, increases allocation and hashing, and allows
partial states.

Rule: C++ hot paths use typed structs, enums, handles, spans, and fixed binary
records. Godot dictionaries are adapters at the outer scripting boundary only.

### GPU use created readback pressure

GPU meshing can be fast, but terrain meshes and collision often return to the
CPU. Synchronous or frequent buffer readback can dominate the pipeline.

Rule: begin with deterministic native CPU Transvoxel meshing. Use compute for
density generation and workloads that remain GPU-resident. Add a GPU meshing
backend only after measurements justify the readback and collision strategy.

### Baking formats grew from runtime objects

The existing system improved toward binary caching, but still combines PNG,
JSON, `store_var`, raw bytes, cache signatures, and runtime object structures.

Rule: define the binary schema before the baker. Use fixed headers, typed
sections, explicit versions, checksums, and migration tools.

### Settings became an unstructured API

More than one hundred exported settings expose internal mechanisms and create
invalid combinations.

Rule: expose a small number of versioned configuration resources. Validate
them once and convert them into immutable native runtime configuration.

### Tests and documentation drifted from scale

The architectural description became stale as the implementation grew.

Rule: CI records source metrics, checks file-size ceilings, verifies manifests,
and generates API/reference tables from source where possible.

## Required architectural response

- One stable public facade, not one implementation file.
- Multiple internal services with explicit ownership.
- Typed commands and events.
- Versioned binary storage.
- Backend separation for MIT, 0BSD, CPU, and future compute implementations.
- Compile-time and CI-enforced source limits.
- A finite milestone and acceptance matrix.
