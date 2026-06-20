# Voxel Tools Findings

Reviewed at Voxel Tools 1.6, revision
`595f52ee4e23203a865eeb981f115909f7aa92f4`.

Voxel Tools is a production reference, not a source template. Its strongest
lesson is the separation of storage, streaming, meshing, task execution, GPU
work, terrain nodes, and Godot resource application.

## Patterns to retain

- Typed threaded tasks expose execution, result application, priority, and
  cancellation separately.
- GPU work has a dedicated runner because RenderingDevice operations can block
  and have thread-affinity constraints.
- GPU outputs are batched to reduce synchronization and readback overhead.
- Transvoxel meshing uses reusable thread-local state rather than allocating
  all scratch data for each cell or block.
- Storage serialization, region persistence, compression, and runtime terrain
  ownership are distinct responsibilities.
- Visual meshing and collision have different constraints and should not be
  treated as one interchangeable output.

## Failure modes to test explicitly

- Degenerate and extremely thin triangles can create physics failures even
  when the visual mesh appears correct.
- Density padding and transition sampling rules affect seams.
- Edits can invalidate more than the directly edited chunk, including lower
  LOD and transition dependents.
- Cancellation must prevent stale work from being applied after viewer motion,
  edits, or eviction.
- RenderingDevice readback must not block the frame thread.

These become contract and integration tests, not comments left inside one
terrain controller.

## Size warning

Subsystem separation alone does not guarantee maintainability. At the reviewed
revision:

```text
terrain/variable_lod/voxel_lod_terrain.cpp       4,171 lines
meshers/transvoxel/transvoxel.cpp                1,555 lines
storage/voxel_data.cpp                           1,355 lines
meshers/transvoxel/transvoxel_tables.cpp         1,081 lines
meshers/mesh_block_task.cpp                        658 lines
meshers/transvoxel/voxel_mesher_transvoxel.cpp     589 lines
```

The new addon therefore keeps the subsystem model but also enforces source
size limits and responsibility reviews. Vendored and generated topology tables
are isolated exceptions.

## Resulting decisions

1. Establish the exact CPU backend before adding compute acceleration.
2. Use immutable typed jobs, generation tokens, explicit cancellation, and
   bounded apply queues.
3. Give GPU submission and readback a dedicated service and thread.
4. Keep storage codecs independent from runtime chunk ownership.
5. Test visual topology, collision suitability, edit invalidation, and seams
   as separate contracts.
6. Reuse architectural lessons without importing Voxel Tools code or license
   obligations.
