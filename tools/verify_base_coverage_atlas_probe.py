#!/usr/bin/env python3
"""Validate the bounded, offline GPU base-coverage atlas probe format."""

from __future__ import annotations

import argparse
import hashlib
import math
import struct
from pathlib import Path


def verify(path: Path) -> dict[str, int]:
    blob = path.read_bytes()
    header = struct.Struct("<4sIQIIIIIiIIII")
    if len(blob) < header.size:
        raise ValueError("atlas header is truncated")
    (
        magic, version, source_revision, seed, mode, count_x, count_y,
        count_z, origin_y, bottom_policy, bottom_thickness, lod, root_count,
    ) = header.unpack_from(blob)
    if (magic, version, source_revision, seed, mode, count_x, count_y,
            count_z, origin_y, bottom_policy, bottom_thickness, lod) != (
            b"WTBA", 1, 190327, 19023, 4, 128, 16, 128, -8, 2, 16, 3
    ):
        raise ValueError("atlas source identity does not match g23")
    if not 1 <= root_count <= 512:
        raise ValueError("invalid root count")
    record = struct.Struct("<iiiIIII32s")
    vertex = struct.Struct("<ffffffHB")
    cursor = header.size
    empty_count = 0
    vertex_count = 0
    index_count = 0
    seen: set[tuple[int, int, int]] = set()
    for _ in range(root_count):
        if cursor + record.size > len(blob):
            raise ValueError("atlas record is truncated")
        x, y, z, flags, vertices, indices, payload_bytes, digest = record.unpack_from(
            blob, cursor
        )
        cursor += record.size
        if (x, y, z) in seen or not (0 <= x < 16 and -1 <= y <= 0 and 0 <= z < 16):
            raise ValueError("duplicate or invalid LOD3 root key")
        seen.add((x, y, z))
        if flags not in (0, 1) or payload_bytes != vertices * vertex.size + indices * 4:
            raise ValueError("invalid record shape")
        if bool(flags) != (vertices == 0 and indices == 0):
            raise ValueError("empty certificate disagrees with geometry")
        if indices % 3:
            raise ValueError("triangle index count is not divisible by three")
        if cursor + payload_bytes > len(blob):
            raise ValueError("atlas payload is truncated")
        payload = memoryview(blob)[cursor:cursor + payload_bytes]
        if hashlib.sha256(payload).digest() != digest:
            raise ValueError("atlas record digest mismatch")
        for offset in range(0, vertices * vertex.size, vertex.size):
            *components, material, authored = vertex.unpack_from(payload, offset)
            if not all(math.isfinite(value) for value in components):
                raise ValueError("nonfinite mesh vertex")
            if authored not in (0, 1):
                raise ValueError("invalid authored-material flag")
            del material
        for (index,) in struct.iter_unpack("<I", payload[vertices * vertex.size:]):
            if index >= vertices:
                raise ValueError("out-of-range mesh index")
        cursor += payload_bytes
        empty_count += flags
        vertex_count += vertices
        index_count += indices
    if cursor != len(blob):
        raise ValueError("atlas has trailing bytes")
    return {
        "roots": root_count,
        "empty": empty_count,
        "vertices": vertex_count,
        "indices": index_count,
        "bytes": len(blob),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("atlas", type=Path)
    args = parser.parse_args()
    result = verify(args.atlas)
    print("WT_BASE_ATLAS_VERIFY_PASS " + " ".join(
        f"{key}={value}" for key, value in result.items()
    ))


if __name__ == "__main__":
    main()
