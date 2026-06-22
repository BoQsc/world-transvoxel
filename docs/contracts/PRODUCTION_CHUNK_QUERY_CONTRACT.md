# Production Chunk Query Contract

Status: implemented; first PQ2 unit

## Scope

`WorldTransvoxelTerrain.query_chunk_state(chunk_coordinate, lod)` returns an
immutable `WorldTransvoxelChunkState` snapshot of main-thread application
readiness. The snapshot exposes:

- requested chunk coordinate and LOD;
- whether an active application record exists;
- current generation token;
- visual readiness;
- whether collision is required;
- collision readiness;
- combined readiness.

The query copies state. It does not retain an internal container pointer, and
later replacement or eviction cannot mutate an earlier result.

## Semantics

A valid but inactive key returns a non-null snapshot with `is_present() ==
false`, generation zero, and all readiness flags false. An invalid LOD returns
null. Coordinates are signed `Vector3i`; valid LODs are `0..20`.

`is_present()` means the production application layer currently expects that
chunk generation. It does not mean a key merely exists in the baked manifest.
Visual readiness means its render payload was applied, including a valid empty
surface. Therefore a ready active chunk can intentionally have no
`MeshInstance3D`. Collision readiness follows the same payload semantics.

Generation is an opaque runtime identity. Callers may compare nonzero values
for equality to detect replacement, but must not persist or predict them.

The method is a Godot main-thread capability over native application state.
It performs a bounded binary search and does not scan viewers, pages, or the
world. Scalar/material sampling and authoritative edit queries remain later
PQ2 work.

## Proof

`production_chunk_query_test.gd` proves on Godot 4.6.3 and 4.7 with debug and
release addon builds:

- valid absent and invalid-LOD behavior;
- fine and coarse active readiness;
- an active ready chunk with an empty render surface;
- collision-required and combined readiness;
- transition-mask replacement changes generation;
- prior snapshots remain unchanged;
- viewer eviction removes the current application record;
- clean lifecycle shutdown.

The underlying sorted application records, generation replacement, stale
result rejection, and independent visual/collision readiness are already
covered by the M3 and M5 native contracts.

## Exact remaining PQ2 sequence

1. complete: immutable active-chunk/readiness query snapshots;
2. complete: lifecycle-owned journal loading and atomic public transactions;
3. complete: apply journal commands to decoded pages, remesh affected
   generations, and replay them after restart;
4. next: expose authoritative scalar/material query plus compaction and
   migration workflow.

PQ2 closes only when a real scene edit survives save/reload and returns
identical authoritative query results.
