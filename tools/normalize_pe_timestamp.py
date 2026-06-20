#!/usr/bin/env python3

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def normalize_timestamp(path: Path) -> None:
    with path.open("r+b") as binary:
        if binary.read(2) != b"MZ":
            raise ValueError(f"{path} is not a PE file")

        binary.seek(0x3C)
        pe_offset_data = binary.read(4)
        if len(pe_offset_data) != 4:
            raise ValueError(f"{path} has a truncated DOS header")
        pe_offset = struct.unpack("<I", pe_offset_data)[0]

        binary.seek(pe_offset)
        if binary.read(4) != b"PE\0\0":
            raise ValueError(f"{path} has no PE signature")

        timestamp_offset = pe_offset + 8
        binary.seek(timestamp_offset)
        timestamp_data = binary.read(4)
        if len(timestamp_data) != 4:
            raise ValueError(f"{path} has a truncated COFF header")
        previous_timestamp = struct.unpack("<I", timestamp_data)[0]

        if previous_timestamp != 0:
            binary.seek(timestamp_offset)
            binary.write(b"\0\0\0\0")

    print(f"PE timestamp normalized: {path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="+", type=Path)
    arguments = parser.parse_args()

    for path in arguments.paths:
        normalize_timestamp(path.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
