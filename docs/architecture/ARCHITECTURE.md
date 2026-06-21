# Architecture

## Primary decision

All terrain complexity belongs to the self-contained addon:

```text
addons/world_transvoxel/
```

The root `world_transvoxel/` directory is reserved for thin application-level
usage: scenes, project configuration, examples, and game-specific adapters. It
must not contain meshing, streaming, storage, collision, or scheduler logic.

## Execution model

```text
Godot scene/editor
    |
    v
WorldTransvoxelTerrain facade
    |
    +-- native world state and event bus
    +-- native stream scheduler and job graph
    +-- native voxel/data source
    +-- native MIT Transvoxel CPU backend
    +-- native mesh/collision builders
    +-- native binary storage and edit journal
    +-- native telemetry
    +-- optional compute services
```

Scene-tree mutation remains on the Godot main thread. Sampling, meshing,
compression, binary I/O, collision-face generation, and scheduling run in
native worker jobs.

## Intended addon layout

```text
addons/world_transvoxel/
  plugin.cfg
  world_transvoxel.gdextension
  LICENSE_SCOPE.md
  thirdparty/
    transvoxel_mit/
      LICENSE
      UPSTREAM.md
      Transvoxel.cpp
  src/
    api/
      world_transvoxel_terrain.*
      world_transvoxel_config.*
      world_transvoxel_data_source.*
      world_transvoxel_edit_api.*
      world_transvoxel_query_api.*
      world_transvoxel_bake_api.*
      world_transvoxel_telemetry.*
    core/
      wt_chunk_key.*
      wt_chunk_state.*
      wt_generation_token.*
      wt_event.*
      wt_error.*
    backend/
      wt_meshing_backend.*
      wt_transvoxel_mit_backend.*
      wt_backend_registry.*
    meshing/
      wt_regular_mesher.*
      wt_transition_mesher.*
      wt_vertex_cache.*
      wt_mesh_buffers.*
      wt_normal_builder.*
    streaming/
      wt_stream_scheduler.*
      wt_viewer_state.*
      wt_lod_map.*
      wt_chunk_priority.*
      wt_job_graph.*
    storage/
      wt_async_storage_service.*
      wt_world_reader.*
      wt_world_writer.*
      wt_chunk_codec.*
      wt_edit_journal.*
      wt_schema.*
    edit/
      wt_edit_command.*
      wt_edit_index.*
      wt_edit_executor.*
    render/
      wt_render_resource.*
      wt_render_apply_queue.*
      wt_material_payload.*
    physics/
      wt_collision_builder.*
      wt_collision_apply_queue.*
    gpu/
      wt_compute_context.*
      wt_density_compute.*
      wt_bake_compute.*
    bake/
      wt_world_baker.*
      wt_bake_manifest.*
    telemetry/
      wt_metrics.*
      wt_trace.*
  shaders/
    wt_density_generate.glsl
    wt_density_edit.glsl
    wt_material_surface.gdshader
    wt_debug_lod.gdshader
  editor/
    world_transvoxel_plugin.gd
  docs/
  tests/
```

## CPU and GPU responsibilities

### Initial production baseline

- Native CPU density sampling for tests and headless operation.
- Native CPU regular and transition meshing using official MIT tables.
- Native CPU collision-face generation.
- Native worker pool with immutable job inputs and generation tokens.
- Main-thread render and physics application queues.
- Separate visual and collision output validation, including thin-triangle
  sanitation for physics.
- Reusable thread-local meshing scratch state.

This path is deterministic, testable without a GPU, and does not pay GPU
readback costs for every chunk.

### Compute shader use

Compute is appropriate for:

- large procedural density batches;
- offline map and material baking;
- broad edit-mask application;
- GPU-resident culling or future visual-only meshing;
- operations whose outputs remain GPU-resident long enough to amortize
  transfers.

Compute is not automatically appropriate for:

- small edits;
- collision-required meshes that immediately return to CPU;
- per-frame counters requiring synchronous readback;
- ownership and scheduling decisions.

RenderingDevice submission and readback use a dedicated native GPU task
service. Results are batched where possible and are returned through bounded
completion queues; the Godot frame thread never waits for a compute readback.

## Event-driven runtime

The scheduler consumes explicit events:

```text
viewer_changed
data_source_invalidated
edit_committed
chunk_loaded
sampling_completed
meshing_completed
collision_completed
render_applied
chunk_eviction_requested
```

Each chunk has a generation token. Late worker results are discarded if their
token no longer matches. Dirty queues contain only affected chunks. There is no
full active-chunk scan in idle frames.

The M5 storage baseline owns an immutable world manifest and resolves
content-addressed chunk objects on a sleeping native I/O worker. Bounded
request and completion queues provide backpressure; completions retain the
request generation for later scheduler-side stale-result rejection.

## Ownership

- World state owns chunk records.
- Stream scheduler owns desired LOD and job issuance.
- Data source owns scalar/material sampling.
- Meshing backend owns topology generation only.
- Render service owns mesh resource creation and visibility application.
- Physics service owns collision resource creation and activation.
- Storage service owns schemas, files, caches, and migrations.
- Edit service owns commands and spatial indexing.
- GDScript owns no runtime terrain arrays or worker queues.
