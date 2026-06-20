# World Transvoxel

Production-oriented Godot terrain research and implementation using the
official upstream MIT Transvoxel tables behind an isolated native addon.

## Canonical direction

Read [IMPLEMENTATION_CHARTER.md](IMPLEMENTATION_CHARTER.md) before changing the
project. It is the single authoritative statement of the intended product,
license boundaries, architecture, performance model, implementation roadmap,
acceptance tests, and production finish line.

If another project document conflicts with the charter, the charter controls.

## Current phase

R0 through M3 are complete. The addon now has the exact native cell backend,
closed-seam chunk meshing, bounded scheduling, Godot `ArrayMesh` rendering,
sanitized concave collision, generation-checked application, and readiness
telemetry. M4 storage, baking, and editing is active. The versioned bounded
container, standalone authoritative chunk pages, native deterministic page
baker, content-addressed world manifest/index, and SHA-256 corruption checks
are implemented. Atomic typed edit transactions are also implemented; spatial
invalidation now covers padded same-LOD and coarser-LOD dependencies. Journal
replay and compaction remain. Production streaming is later work.

## Tooling

All project-owned build, download, validation, and test automation uses Python
3.11 or newer. The entry points share host detection and path/process safety;
there are no operating-system shell scripts in the project-owned toolchain.

```console
python scripts/bootstrap_toolchain.py
python scripts/build.py
python scripts/test_m4.py
```

## Production strategy

1. Start with the official upstream MIT `Transvoxel.cpp`.
2. Keep it unchanged and isolated inside `addons/world_transvoxel/thirdparty/`.
3. Access it through a project-owned backend interface.
4. Establish a long-running production baseline for terrain, collision,
   editing, LOD, streaming, and performance.
5. Keep alternate backends, including the independent 0BSD implementation,
   behind the same interface.
6. Switch only after an alternate backend passes the same acceptance suite.

## Engineering direction

- Godot GDExtension C++ owns hot paths, data, scheduling, meshing, collision
  preparation, storage, and telemetry.
- Compute shaders are used where work remains GPU-resident or is naturally
  batched. GPU readback is not treated as free.
- GDScript is limited to editor integration, scene wiring, examples, and
  non-critical scaffolding.
- World and edit data use explicit versioned binary formats.
- Runtime work is event-driven and budgeted; idle frames do not scan every
  chunk.
- Public API is intentionally small. Internally, responsibilities are split
  into typed services with predictable ownership.
- Source files are prefixed and size-limited. Vendor and generated files are
  isolated exceptions.

## Intended repository layout

```text
addons/world_transvoxel/       self-contained native addon
world_transvoxel/              thin example/application integration
docs/                          architecture, formats, decisions, roadmap
references/                    pinned source manifest and local downloads
tests/                         unit, exhaustive, integration, soak, performance
tools/                         bake, inspect, validate, and conversion tools
```

Start with:

```text
IMPLEMENTATION_CHARTER.md
docs/research/MARCHING_CUBES_POSTMORTEM.md
docs/research/REFERENCE_ASSESSMENT.md
docs/research/VOXEL_TOOLS_FINDINGS.md
docs/architecture/ARCHITECTURE.md
docs/architecture/API_BOUNDARIES.md
docs/architecture/BINARY_FORMATS.md
docs/architecture/CODING_STANDARDS.md
docs/ROADMAP.md
```

Download the pinned local research set:

```console
python scripts/download_references.py
```

Validate the repository and any locally downloaded references:

```console
python tools/validate_repository.py
```
