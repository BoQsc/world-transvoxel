# World Transvoxel Addon

This directory is the self-contained production addon boundary.

M0 provides:

- an isolated official MIT Transvoxel table source;
- a project-owned native backend interface and minimal adapter;
- a minimal `WorldTransvoxelTerrain` GDExtension class;
- Zig-driven Windows x86-64 debug and release builds.

M0 does not implement terrain cells, chunks, streaming, rendering, collision,
editing, baking, or compute.

Build from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build.ps1
```

Validate M0:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/test_m0.ps1
```
