# Coding Standards

## Naming

- Public Godot classes: `WorldTransvoxel*`.
- Public source files: `world_transvoxel_*`.
- Internal C++ symbols and files: `Wt*` and `wt_*`.
- Shader files: `wt_*`.
- Test files: `test_wt_*`.
- Binary tools: `wt_*`.

Generic names such as `manager`, `utils`, `common`, and `mesh_builder` require
a narrower qualifier.

## Source size

Soft limits:

```text
C++ implementation       600 lines
C++ header               300 lines
GDScript                 250 lines
shader                   400 lines
```

Hard CI limits:

```text
C++ implementation       900 lines
C++ header               450 lines
GDScript                 400 lines
shader                   650 lines
```

Vendor, generated table, and generated binding files are exempt only when they
live in explicitly excluded directories.

A file approaching the soft limit requires a responsibility review. Splitting
by arbitrary line ranges is not acceptable; split by ownership and contract.

## Dependency direction

```text
api -> services -> core
services -> backend interfaces
backend implementations -> backend interfaces + core
storage/render/physics/gpu do not depend on gameplay
thirdparty does not include project headers
```

Circular includes and cross-service singleton lookups are forbidden.

## Runtime rules

- No scene-tree searches in native hot paths.
- No global mutable terrain dictionary.
- No blocking GPU readback on the frame thread.
- No background thread touching scene-tree objects.
- No per-frame full scan to find dirty chunks.
- No `Variant` or string-key task payloads inside worker queues.
- No unversioned persistent format.
- No modification of vendored upstream files.

## Automation

- Project-owned build, download, validation, and test entry points are Python
  3.11+ programs.
- Shared path safety, host detection, hashing, downloading, and subprocess
  behavior belongs in `scripts/wt_script_common.py`.
- Automation must use `pathlib` and argument arrays, not shell command strings.
- Host support is detected explicitly; a script must fail clearly for an
  unsupported host instead of silently selecting another platform.

## Documentation and tests

- Public classes receive GDExtension XML documentation.
- Architecture metrics are generated in CI.
- Each subsystem has unit tests.
- Every backend runs the same contract suite.
- Performance gates use representative motion and editing, not idle-only
  measurements.
