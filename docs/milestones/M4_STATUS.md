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
M4_BAKE_HASH       8945e3394a7ee1bcf4f7eec0a07da792ad8296ab35ef8f3413e11cc1b1fcf5bf
M4_WORLD_HASH      b7b4493e4b0fc5275b283117abbfb0c4fb57f8fc53729e047b895f30155bcf17
M4_EDIT_HASH       b6486070db6d6d3083dc5858d72bbfeebf3b4c005167cc568ac22cff2261c19c
M4_SPATIAL_HASH    dd58c70452ae48e8e32d582d769c13ccfe235b64d612aba668e4ad15d89ef513
M4_JOURNAL_HASH    3c45936ac7e05e0736e0ae4b432e634cc4b587a9c498f4b2eebb7942532508d8
M4_APPLY_HASH      718299711f482dec5c606b31fefa61e6576d73f4226c0b84c5059e70031eded1
M4_COMPACTION_HASH 73f0f8ac46d1dfc3d235788b8c9d04a90934b0d70bf94aedf44a09e3562fe049
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
