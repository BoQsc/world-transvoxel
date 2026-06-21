# M4 Storage Container Contract

Status: normative M4 foundation

## Format family

The common container is used by:

```text
WTWORLD\0  *.wtworld
WTCHUNK\0  *.wtchunk
WTEDIT\0\0 *.wtedit
WTTRACE\0  *.wttrace
```

All integer fields are little-endian fixed-width values. Native structure
layout, pointers, filesystem timestamps, Godot variants, and backend topology
data are forbidden.

## Version 1 common header

The common header is exactly 80 bytes:

```text
offset  size  field
0       8     magic
8       2     format_major = 1
10      2     format_minor = 0
12      4     header_size = 80
16      8     feature_flags
24      8     source_revision
32      8     directory_offset = 80
40      8     directory_size
48      32    SHA-256(directory through end of file)
```

Feature bits `0..31` are optional. Unknown optional bits are preserved and
ignored by readers. Bits `32..63` are required features; an unknown required
bit fails before directory or payload use.

Major version changes are incompatible. A reader accepts its current major
and a minor version no newer than its own. Migration tools, rather than silent
reinterpretation, handle old incompatible versions.

## Version 1 section directory

Entries are sorted by unsigned four-character section type and are exactly 68
bytes:

```text
offset  size  field
0       4     type/fourcc
4       4     section flags
8       2     codec
10      2     reserved = 0
12      8     payload offset
20      8     stored size
28      8     uncompressed size
36      32    SHA-256(uncompressed section payload)
```

Version 1 supports codec `0` (`none`) only. Stored and uncompressed sizes must
therefore match. Duplicate types, unsorted entries, gaps, overlaps, trailing
unowned bytes, unknown codecs, nonzero reserved fields, and invalid ranges
fail. Successful section payloads are immutable views into caller-owned bytes,
not copied allocations.

## Bounded behavior

```text
maximum container size  256 MiB
maximum section size     64 MiB
maximum section count      4096
```

Writers calculate every size with checked arithmetic before allocation.
Readers validate the complete container hash before parsing section contents,
then validate each section hash before exposing its view. Failed reads clear
all output views.

These limits bound individual files and pages. A baked world larger than one
container uses indexed range-readable chunk pages; it does not increase these
limits or require loading the world into one allocation.

## Current proof

`tests/native/test_wt_m4_storage.cpp` covers standard empty and `abc` SHA-256
vectors, little-endian primitive round trips, writer capacity, reader
truncation, deterministic section ordering, zero-copy views, and corruption of
magic, version, required features, codec, section bounds, duplicate types,
container payload, and truncation.

The common container is one component of completed M4. Chunk pages, the world
manifest/index, atomic edit transactions, journal replay, compaction,
migration, inspection, and baking all use or validate it.
