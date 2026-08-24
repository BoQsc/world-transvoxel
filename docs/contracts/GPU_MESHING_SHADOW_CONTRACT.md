# GPU Meshing Shadow Contract

Status: `TQP64_LIVE_SHADOW_VALIDATION`

The GPU meshing shadow is an opt-in diagnostic boundary for comparing a GPU
candidate against accepted live CPU terrain work. It does not replace CPU
meshing, rendering, collision generation, world state, edits, scheduling, or
publication.

## Capture

When disabled, live meshing uses the existing backend path and no cell capture
callback is installed. When enabled, each accepted terrain or static-water
mesh job records its exact regular and transition cell inputs together with
the CPU backend result. Capture occurs only after the native runtime accepts
the job's generation, source revision, world revision, and lifecycle state.

The queue is explicitly bounded. Capacity includes queued and in-flight
requests. Saturation increments `capacity_rejections`; it never blocks a
meshing worker and never changes the CPU result.

When capacity is full, a newer source/world revision or a newer generation of
the same page may replace older work that is still queued. In-flight work is
never revoked and still completes through normal stale validation. Queue
capacity does not grow; `superseded_queued_requests` records every replacement.

## Identity

Every request carries:

- chunk coordinate and LOD;
- generation token;
- source revision and world revision;
- transition mask and cached transition mask;
- terrain or static-water surface identity;
- field mode and sample count.

The consumer must return the exact request identity. Changed identity is
rejected. A result is stale when its source/world revision is no longer
current or a newer generation/revision for the same chunk is queued or in
flight.

## Publication

GPU shadow completion records only `MATCHED`, `MISMATCHED`, `STALE`,
`UNKNOWN_REQUEST`, or `IDENTITY_MISMATCH`. It has no geometry publication
method. The native CPU terrain mesh and targeted CPU collision mesh continue
through their existing publication path whether capture succeeds, saturates,
or is disabled.

## Qualification

Required coverage includes:

1. native queue capacity, identity, stale, and mismatch tests;
2. unchanged CPU page meshing, streaming, and LOD streaming tests;
3. live Godot streaming with terrain and collision resources ready;
4. live construct/remesh and volumetric static-water meshing;
5. Vulkan and D3D12 GPU differential agreement;
6. a deliberately superseded live result rejected before publication.

This contract does not qualify GPU-resident rendering, persistent shared GPU
buffers, production GPU publication, collision readback, recovery, large-world
performance, or release promotion.
