# M4 Edit Transaction Contract

Status: normative M4 component

A `wtedit` schema-1 container stores one complete atomic edit transaction.
Transactions are backend-neutral authoritative commands. They do not contain
meshes, case IDs, Godot variants, pointers, or Transvoxel lookup data.

This format is the atomic unit later used by the append-only journal and
compactor. A transaction is valid only when its header, ordered command
records, per-record hashes, and commit footer all agree.

## Coordinate and operation model

Shape coordinates use signed Q16 base-grid units:

```text
stored coordinate / 65536 = base-grid coordinate
```

Density values use finite IEEE-754 binary32. Materials are categorical
`uint16_t`.

Schema 1 operations are:

| Value | Operation | Value rules |
| ---: | --- | --- |
| 1 | add density | finite, nonzero density; material zero |
| 2 | set density | finite density; material zero |
| 3 | paint material | density zero; material is the target |

Schema 1 shapes are:

| Value | Shape | Payload |
| ---: | --- | --- |
| 1 | sphere | Q16 center X/Y/Z and positive Q16 radius |
| 2 | axis-aligned box | Q16 minimum and maximum X/Y/Z |

Every command stores exact conservative signed-64-bit integer bounds. Minimum
bounds are the floor of the Q16 shape minimum; maximum bounds are the ceiling
of the Q16 shape maximum. Writers and readers recompute and require those
bounds. Overflowing spheres and inverted boxes fail.

## Container sections

The common magic is `WTEDIT`. Feature flags are zero. Exactly three
codec-`none`, flag-zero sections are required:

| FourCC | Purpose |
| --- | --- |
| `EHDR` | transaction identity and revisions |
| `CMDS` | ordered typed command records |
| `CMIT` | atomic commit footer |

The common source revision must equal `EHDR.source_revision`.

## `EHDR` schema 1.0

`EHDR` is exactly 64 bytes:

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | `u16` | schema major, `1` |
| 2 | `u16` | schema minor, `0` |
| 4 | `u8[16]` | nonzero transaction ID |
| 20 | `u64` | baked source revision |
| 28 | `u64` | base world edit revision |
| 36 | `u64` | committed world edit revision |
| 44 | `u64` | author ID |
| 52 | `u32` | command count, `1..4096` |
| 56 | `u32` | reserved, zero |
| 60 | `u32` | reserved, zero |

The committed revision must equal `base_revision + 1`, without overflow.

## `CMDS` records

The section begins with schema major/minor and a `u32` command count. Each
record is self-sized and contains a 108-byte common header:

```text
record_size:u32
schema_major:u16
schema_minor:u16
command_id:u8[16]
sequence:u32
world_revision:u64
operation:u16
shape:u16
flags:u32
density_value:f32
material:u16
reserved:u16
bounds_min_x/y/z:i64
bounds_max_x/y/z:i64
shape_payload_size:u32
reserved:u32
```

The typed shape payload follows:

- sphere: 32 bytes, `center_x/y/z:i64` and `radius:u64`;
- box: 48 bytes, `minimum_x/y/z:i64` and `maximum_x/y/z:i64`.

A 32-byte SHA-256 over the complete record header and payload terminates each
record. `record_size` includes that hash.

Commands are ordered by contiguous sequence `0..count-1`. Every command ID is
nonzero and unique, and every command world revision equals the transaction's
committed revision. Input vector order does not affect serialized bytes.

## `CMIT` atomic footer

`CMIT` is exactly 68 bytes:

```text
schema_major:u16
schema_minor:u16
transaction_id:u8[16]
committed_revision:u64
command_count:u32
reserved:u32
commands_hash:u8[32]
```

The transaction ID, revision, and count must match `EHDR`.
`commands_hash` is SHA-256 of the entire `CMDS` section, including its schema
header and all record hashes. Missing, mismatched, or partially replaced
commands therefore cannot commit.

## Determinism and evidence

`tests/native/test_wt_m4_edit.cpp` locks this debug/release transaction hash:

```text
b8d28a739463c3e43a20d14f9d0496d3041c8e667e77f1e5f029256855a2b26d
```

The test covers both shapes, all three operations, negative fractional
coordinates, exact conservative bounds, input-order independence,
byte-identical round-trip, revision and sequence rules, duplicate IDs,
non-finite and zero-strength values, invalid shape bounds, container
corruption, section flags, and commit-to-command hash disagreement.

This component does not complete M4. The bounded spatial invalidation index,
append-only journal replay, transaction ordering, compaction, authoritative
sample reconstruction, migrations, and user-facing bake/inspect tools remain.
