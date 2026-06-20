# World Transvoxel Addon

This directory is the self-contained production addon boundary.

M0 through M2 provide:

- an isolated official MIT Transvoxel table source;
- a project-owned typed, fixed-capacity native backend interface;
- official regular and transition-cell meshing;
- all six right-handed transition orientations;
- reusable thread-local scratch storage;
- exhaustive debug/release native contract tests and deterministic hashes;
- signed chunk keys, 2:1 LOD validation, and deterministic transition ownership;
- bounded event-driven jobs, cancellation, and stale-result rejection;
- regular chunk meshing and all face, edge, and corner seam galleries;
- a minimal `WorldTransvoxelTerrain` GDExtension class;
- host-aware Python build orchestration using Zig and SCons;
- validated Windows x86-64 debug and release builds.

M2 does not create Godot mesh resources or implement collision, editing,
storage, production streaming, baking, or compute.

The active M3 work now includes native render payload preparation, collision
sanitation, bounded main-thread application queues, and generation-checked
readiness. Godot mesh/physics resource creation is not complete yet.

Build from the repository root:

```console
python scripts/build.py
```

Validate the active M3 foundation and all prior milestone contracts:

```console
python scripts/test_m3.py
```
