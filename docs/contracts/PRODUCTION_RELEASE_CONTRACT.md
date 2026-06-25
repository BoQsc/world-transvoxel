# Production Release Contract

Status: normative PQ4 gate

World Transvoxel 1.0.0 is withdrawn because it had incorrect Godot-facing
render/collision winding and incomplete convex mixed-LOD corner deformation.

World Transvoxel 1.0.1 is superseded because it could expose transient holes
during moving-viewer LOD replacement. World Transvoxel 1.0.2 is superseded by
1.0.3's bounded bake lifetime. World Transvoxel 1.0.3 is superseded because
dynamic mixed-LOD viewer motion could retire a large ready replacement set in
one frame. World Transvoxel 1.0.5 also applies a fixed native render fade-out to
retiring chunks after replacement application. World Transvoxel 1.0.6 also
applies a fixed native render fade-in to newly introduced chunks. World
Transvoxel 1.0.6 is
released as a deterministic directory,
not an archive:

```text
artifacts/release/world-transvoxel-1.0.6-windows-x86_64/
```

The directory contains the complete `addons/world_transvoxel` install,
runtime debug/release DLLs, packaged release/debug bake and storage tools,
root 0BSD and MIT notices, and `RELEASE_MANIFEST.json`.

The release GDExtension descriptor advertises only the qualified Windows
x86-64 debug/release mappings. Source-checkout mappings for unqualified future
targets are not copied into the release descriptor.

## Determinism

`scripts/test_pq4.py` removes only verified release build outputs, performs
two independent Zig/SCons builds, materializes two release directories, and
compares every relative path and every byte. PE timestamps are normalized by
the build. The manifest contains no timestamp, absolute path, or mutable Git
metadata.

The manifest records stable product/toolchain/backend identity plus path,
size, and SHA-256 for every payload file. A deterministic content-root hash
covers the same ordered file set.

The directory contains no compiler object, static/import library, linker
database, or debug-symbol intermediate. PQ4 compares the complete payload to
the manifest in both directions, so unlisted files also fail qualification.

## Installed boundary

The installed editor defaults to:

```text
res://addons/world_transvoxel/tools/wt_bake.py
```

Both packaged Python tools locate native executables under the addon's `bin/`
directory and have no repository script/build dependency. Root `tools/`
scripts are compatibility shims for source checkouts.

## Acceptance

PQ4 passes only when:

- the two clean release directories are byte-identical;
- the payload exactly matches the manifest and contains no native build
  intermediates;
- all required 0BSD/MIT notices and pinned upstream bytes pass audit;
- the packaged bake and storage tools produce and validate a real world;
- ready chunk retirement removal is bounded per frame in the native runtime;
- retiring render chunks have a bounded native fade-out window;
- newly introduced render chunks have a bounded native fade-in window;
- the exact release addon passes the complete PQ3 Godot 4.6.3/4.7
  debug/release clean-install workflow;
- public API, supported matrix, and operational limits ship inside the addon.

Reference evidence is
`docs/evidence/pq4_release_windows_x86_64.json`.
