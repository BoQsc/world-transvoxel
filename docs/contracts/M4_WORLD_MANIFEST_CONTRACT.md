# M4 World Manifest and Page Index Contract

Status: normative M4 component

An immutable `wtworld` manifest records the identities needed to reproduce a
bake and maps canonical chunk keys to independently stored `wtchunk` objects.
It does not embed page payloads, generated meshes, Godot resources, or
Transvoxel lookup data.

The page object store is content-addressed by the complete-page SHA-256. A
filesystem, archive, HTTP range service, or application cache may implement
that store without changing this format.

## Container sections

The common container magic is `WTWORLD`. Feature flags are zero for schema
1.0. Exactly three codec-`none`, flag-zero sections are required:

| FourCC | Purpose |
| --- | --- |
| `META` | fixed world and page-schema metadata |
| `DEPS` | sorted source and tool dependency identities |
| `INDX` | sorted chunk-key to page-object index |

Missing, extra, duplicate, or nonzero-flag sections fail. The common
container's source revision must equal the `META` source revision.

## `META` schema 1.1

`META` schema 1.1 is exactly 72 bytes:

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | `u16` | schema major, `1` |
| 2 | `u16` | schema minor, `0` |
| 4 | `u32` | indexed page count |
| 8 | `u32` | dependency count |
| 12 | `u16` | chunk cells per axis, `16` |
| 14 | `u8` | maximum supported LOD, `20` |
| 15 | `u8` | reserved, zero |
| 16 | `u64` | authoritative source revision |
| 24 | `u8[32]` | nonzero configuration SHA-256 |
| 56 | `u16` | required chunk-page schema major, `1` |
| 58 | `u16` | maximum chunk-page schema minor, `0` |
| 60 | `u32` | reserved, zero |
| 64 | `u64` | current authoritative world edit revision |

Page count is bounded to 262,144 and dependency count to 1,024.
Schema 1.0's exact 64-byte `META` remains readable and yields world revision
zero. Writers emit schema 1.1.

## `DEPS` schema 1.1

The section begins with:

```text
schema_major:u16
schema_minor:u16
entry_count:u32
```

Each variable-size entry contains:

```text
kind:u16
flags:u16 = 0
label_size:u16
version_size:u16
content_hash:u8[32]
label:utf8[label_size]
version:utf8[version_size]
```

Labels are nonempty, versions are required except for source assets, and each
text field is at most 255 bytes. UTF-8 must be structurally valid and may not
contain control characters. Content hashes are nonzero.

Entries are sorted by `(kind, label, version, content_hash)`. A `(kind,
label)` identity may appear only once.

Kinds are:

| Value | Kind |
| ---: | --- |
| 1 | source asset; zero or more |
| 2 | generator; exactly one |
| 3 | configuration; exactly one |
| 4 | backend/upstream Transvoxel identity; exactly one |
| 5 | Godot identity; exactly one |
| 6 | godot-cpp identity; exactly one |
| 7 | native toolchain identity; exactly one |

The configuration dependency hash must equal `META.configuration_hash`.
Format versions are represented directly by the common, world, and page
schema fields rather than a dependency string.

## `INDX` schema 1.1

The 12-byte index header is:

```text
schema_major:u16
schema_minor:u16
entry_count:u32
entry_size:u16 = 56
reserved:u16 = 0
```

Each fixed 56-byte entry is:

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | `i32` | chunk X |
| 4 | `i32` | chunk Y |
| 8 | `i32` | chunk Z |
| 12 | `u8` | LOD |
| 13 | `u8` | flags, zero |
| 14 | `u16` | reserved, zero |
| 16 | `u64` | complete `wtchunk` byte size |
| 24 | `u8[32]` | SHA-256 of complete `wtchunk` bytes |

Entries are unique and sorted by canonical chunk order `(lod, z, y, x)`.
Page sizes must fit the common 256 MiB per-container limit. Hashes are
nonzero.

## Resolution and validation

To resolve a page, storage performs a binary search by chunk key, obtains the
standalone object by its content hash, and verifies:

1. fetched byte size equals the index;
2. SHA-256 of all fetched bytes equals the index;
3. the bytes decode as a valid `wtchunk`;
4. the page chunk key equals the requested key;
5. the page source revision equals the world source revision.

Only then may the authoritative samples enter meshing or editing.

## Determinism and evidence

Writers sort dependencies and pages, so input order does not affect bytes.
`tests/native/test_wt_m4_world.cpp` locks the debug/release schema-1.1 world
hash:

```text
2b5281537b9247480431a3a1dc2608e994e3d458f399f236f3a0186536843b2e
```

The test covers schema-1.1 and schema-1.0 migration, manifest round-trip,
canonical ordering, page lookup and
validation, missing/size/hash/metadata failures, incomplete and duplicate
dependencies, configuration disagreement, invalid text, duplicate page keys,
container corruption, section flags, and extra sections.

Typed edit transactions, spatial invalidation, journal replay, authoritative
application, and compaction now consume this manifest. Inspection tools and
command-line/editor baking entry points remain active M4 work.
