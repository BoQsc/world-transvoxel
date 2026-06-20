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

M3 adds deterministic render payloads, Godot `ArrayMesh` resources, sanitized
concave collision, distance hysteresis, bounded main-thread application,
generation-checked readiness, and application-latency telemetry.

M3 does not implement editing, storage, production streaming, baking, or
compute.

Active M4 work includes the versioned bounded `wtworld`/`wtchunk`/`wtedit`/
`wttrace` container and SHA-256 validation. Authoritative chunk and edit
schemas are not complete yet.

Build from the repository root:

```console
python scripts/build.py
```

Validate the active M4 foundation and all prior milestone contracts:

```console
python scripts/test_m4.py
```
