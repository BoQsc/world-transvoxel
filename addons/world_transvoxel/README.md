# World Transvoxel Addon

This directory is the self-contained production addon boundary.

M0 and M1 provide:

- an isolated official MIT Transvoxel table source;
- a project-owned typed, fixed-capacity native backend interface;
- official regular and transition-cell meshing;
- all six right-handed transition orientations;
- reusable thread-local scratch storage;
- exhaustive debug/release native contract tests and deterministic hashes;
- a minimal `WorldTransvoxelTerrain` GDExtension class;
- host-aware Python build orchestration using Zig and SCons;
- validated Windows x86-64 debug and release builds.

M1 does not implement chunks, streaming, rendering, collision, editing, baking,
or compute.

Build from the repository root:

```console
python scripts/build.py
```

Validate M1, including the M0 engine compatibility baseline:

```console
python scripts/test_m1.py
```
