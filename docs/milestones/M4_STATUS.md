# M4 Status: Storage, Baking, and Editing

Status: complete on 2026-06-21

Validated platform:

```text
Windows x86-64
Zig 0.16.0
Godot 4.6.3
Godot 4.7
```

Addon version: `0.5.0-m4`

## Delivered contracts

- bounded little-endian `WTWORLD`, `WTCHUNK`, `WTEDIT`, and common container
  headers/directories;
- SHA-256 whole-container, section, page, world, journal, and audit identities;
- standalone 19-cubed float32/uint16 authoritative chunk pages;
- deterministic native chunk baking and content-addressed world indexing;
- complete source/configuration/backend/Godot/godot-cpp/Zig manifests;
- atomic typed sphere/box density and material transactions;
- padded same-LOD and coarser-LOD spatial invalidation;
- append-only transaction journals with strict load and truncated-tail
  recovery;
- exact native command replay over authoritative chunk samples;
- audited compaction into a new source revision and persisted world revision;
- schema-1.0 world migration to current schema 1.1;
- native-backed Python inspect, validate, migrate, and dense bake tools;
- thin asynchronous Godot editor bake entry;
- schema-1 codec decision: required `none`, no compressed codec admitted.

## Locked native hashes

```text
M4_BAKE_HASH       5f01045e4846d377341a72fff5acb84f80d1b8761fe33a89b3b835c6e1a5ef3f
M4_WORLD_HASH      7fd6b604222c4eee8092d8dea6f52c04a0e10435c3a76f55839f97cf24fc2b16
M4_EDIT_HASH       791614cdc4f30d9d346c7ca11ae520d3b52c6b3a8dc804db12bd6a51cc544301
M4_SPATIAL_HASH    dd58c70452ae48e8e32d582d769c13ccfe235b64d612aba668e4ad15d89ef513
M4_JOURNAL_HASH    df6179fa610be63f60aee9c85af340b81f4a6e785ced4e18d428830e605de25a
M4_APPLY_HASH      79f6ee15201acb6862928f361db1f8900469a56cac48e91cb3cfdbb702938daa
M4_COMPACTION_HASH b4e61fad7fa7281567771d0d698a7234b402aae4d70d49deae3a3b50b5fe02f7
```

Every hash matches between template-debug and template-release builds.

## Exit evidence

The M4 exit condition is:

```text
bake-load-edit-save-reload reproduces identical authoritative state
and all corruption cases fail safely
```

The complete workflow is proven by:

- `test_wt_m4_bake`: input-order-independent pages and byte-identical page
  decode/re-encode;
- `test_wt_m4_world`: indexed page validation and schema-1.0 migration;
- `test_wt_m4_edit`: atomic command records and commit hashes;
- `test_wt_m4_spatial`: padded boundary and dependent-LOD invalidation;
- `test_wt_m4_journal`: ordered append, strict load, recovery, and replay;
- `test_wt_m4_apply`: adjacent-page overlap equality after edits;
- `test_wt_m4_compaction`: direct replay equals compacted/reloaded samples and
  the next journal continues the persisted revision;
- `test_m4_tools.py`: inspect/validate/migrate integration and corruption
  rejection;
- `test_m4_bake_tool.py`: real dense-volume debug/release artifact agreement;
- `m4_editor_bake_test.gd`: shared editor command construction on both Godot
  versions;
- `benchmark_m4_codec.py`: deterministic RLE rejection and `none` decision.

The complete M3, M2, M1, and M0 suites also pass, including debug/release addon
load and M3 render/collision integration on Godot 4.6.3 and 4.7.

Repository validation passes with no source-size or license-boundary warnings.

## Scope boundary

M4 does not claim production streaming. It has no asynchronous page store,
bounded production caches, multi-viewer desired-set integration, eviction
policy, representative motion/edit soak budgets, or telemetry trace schema.

M5 subsequently completed. Production qualification is the next and only
active phase.
