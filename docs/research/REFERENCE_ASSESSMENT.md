# Reference Assessment

Reviewed on 2026-06-20.

## Direct production references

### Eric Lengyel: Transvoxel

The dissertation, article, and official lookup-table repository remain the
directly applicable references for dynamically editable smooth voxel terrain
with seamless 2:1 LOD transitions.

The official repository is a table source, not a complete terrain engine.
Production still requires chunk ownership, sampling, LOD constraints,
transition placement, vertex reuse, normals, collision, edits, streaming,
storage, rendering, and validation.

Decision: use the official tables unchanged as the first production backend.

### Voxel Tools for Godot

Voxel Tools is the most relevant current production reference for Godot. It
implements editable streamed terrain, Transvoxel LOD, physics integration,
storage, generators, viewers, and multithreaded task scheduling.

Decision: study its subsystem boundaries, task scheduling, voxel storage,
viewer model, and tests. Do not copy its implementation into a new monolith.
The concrete code review is recorded in `VOXEL_TOOLS_FINDINGS.md`.

## Alternative meshing research

### Dual Contouring of Hermite Data, 2002

Important for sharp-feature reconstruction and octree contouring. It is not a
drop-in answer for the current requirement because it changes topology,
sampling requirements, and LOD stitching architecture.

### GPU-friendly AMR isosurface extraction, 2020

Supports arbitrary AMR layouts and is highly parallel. It is relevant if the
project later changes from chunked 2:1 terrain to a general adaptive-cell
representation. It does not eliminate streaming, editing, collision, storage,
or Godot integration work.

### McGrids, 2024

Adaptive sampling reduces implicit-field evaluations for reconstruction. Its
main target is efficient extraction from analytical or learned implicit
fields, not continuous real-time remeshing of gameplay terrain.

### TetWeave, 2025

Produces high-quality watertight adaptive meshes through Delaunay
tetrahedralization and optimization. It is aimed at mesh optimization and
reconstruction, not low-latency chunk edits.

### Power-diagram adaptive extraction, 2025

Improves geometric fidelity through iterative adaptive refinement. It is
valuable for offline baking or future high-quality static assets, not the
initial streamed terrain backend.

### Dual Contouring of Signed Distance Data, 2026

Improves sharp-feature reconstruction from discrete SDF samples without
gradient input. It is a current quality reference, but it does not provide the
complete multiresolution terrain, transition, streaming, and collision system.

## Conclusion

No recent paper removes the engineering responsibilities that caused the
existing project to become difficult to maintain. Replacing Transvoxel with a
new extraction paper would reset topology, LOD, collision, and integration
risk.

The stable route is:

1. Pin Transvoxel and a supported Godot/GDExtension toolchain.
2. Use strict architecture boundaries.
3. Establish a deterministic CPU baseline.
4. Add compute only where data movement makes it beneficial.
5. Measure real terrain behavior.
6. Evaluate newer algorithms as explicit alternative backends or offline
   bakers, not as moving production targets.
