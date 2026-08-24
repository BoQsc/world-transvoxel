# GPU Meshing Publication Contract

Status: `TQP64_BOUNDED_MATCHED_CELL_PUBLICATION`

This contract defines the first production-integration publication stage for
GPU-generated Transvoxel cells. It is disabled by default. It does not change
CPU world, edit, revision, storage, scheduling, or collision authority.

## Accepted Route

A visual candidate may enter the render queue only after all of these steps:

1. the native runtime accepts and CPU-meshes an exact page generation;
2. the bounded GPU worker meshes the captured regular and transition cells;
3. the worker-side differential reports an exact accepted match;
4. native code parses the returned GPU cells and runs the existing native
   chunk finalizer against the retained source pages;
5. finalized terrain or static-water geometry exactly equals the captured CPU
   authority mesh and combined render payload;
6. the application record still requires the same visual generation and its
   CPU visual is already ready;
7. the candidate is queued as a same-generation visual replacement.

Any failed step rejects or skips the candidate. There is no fallback GPU mesh,
reinterpreted identity, partial publication, or publication after a mismatch.

## Identity And Ordering

Identity includes chunk X/Y/Z/LOD, generation, source revision, world revision,
transition masks, surface, field mode, and sample inventory. Native queue
freshness validation occurs before finalization. Changed identity is rejected.
Superseded work is stale and never reaches the render queue.

A matched candidate whose application record is no longer current or whose CPU
visual is not ready is recorded as a stale application skip. This is expected
under bounded live streaming and is distinct from finalization, queue, or sink
failure. A candidate accepted by the render queue is validated again against
the current generation when applied.

## Authority

The CPU mesh remains the differential authority and initial visual
publication. Collision payload generation and publication stay on the existing
targeted CPU path. GPU candidates contain render payloads only. Application
metrics distinguish CPU-authority and GPU-cell-candidate render submissions,
applications, and stale results.

## Current Limit

This stage reads cell results back to the CPU, reruns native finalization, and
uploads an ordinary Godot `ArrayMesh`. It is not GPU-resident rendering,
zero-readback publication, indirect production drawing, or a performance
promotion. Those require a separate engine-facing render-resource contract and
remain TQP-64 work.

Required qualification includes native identity/application tests, focused
terrain and static-water Godot smoke tests, deliberate stale rejection, and the
2,048 x 256 x 2,048 relocation/edit route on every retained rendering driver.
