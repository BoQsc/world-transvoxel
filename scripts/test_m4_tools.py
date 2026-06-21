#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from wt_script_common import REPO_ROOT, native_tool_path


def fourcc(value: bytes) -> int:
    return int.from_bytes(value, "little")


def container(
    magic: bytes,
    source_revision: int,
    sections: list[tuple[int, bytes]],
) -> bytes:
    ordered = sorted(sections)
    directory_size = len(ordered) * 68
    offset = 80 + directory_size
    directory = bytearray()
    payload = bytearray()
    for section_type, data in ordered:
        digest = hashlib.sha256(data).digest()
        directory.extend(
            struct.pack(
                "<IIHHQQQ32s",
                section_type,
                0,
                0,
                0,
                offset,
                len(data),
                len(data),
                digest,
            )
        )
        payload.extend(data)
        offset += len(data)
    hashed_payload = bytes(directory + payload)
    header = struct.pack(
        "<8sHHIQQQQ32s",
        magic,
        1,
        0,
        80,
        0,
        source_revision,
        80,
        directory_size,
        hashlib.sha256(hashed_payload).digest(),
    )
    return header + hashed_payload


def legacy_world() -> bytes:
    config_hash = hashlib.sha256(b"configuration").digest()
    entries = [
        (1, "terrain", "", hashlib.sha256(b"terrain").digest()),
        (2, "baker", "1", hashlib.sha256(b"baker").digest()),
        (3, "config", "1", config_hash),
        (4, "mit", "1", hashlib.sha256(b"mit").digest()),
        (5, "godot", "4.6.3", hashlib.sha256(b"godot").digest()),
        (6, "godot-cpp", "e83f", hashlib.sha256(b"godot-cpp").digest()),
        (7, "zig", "0.16.0", hashlib.sha256(b"zig").digest()),
    ]
    entries.sort(key=lambda entry: (entry[0], entry[1], entry[2], entry[3]))
    dependencies = bytearray(struct.pack("<HHI", 1, 0, len(entries)))
    for kind, label, version, digest in entries:
        label_bytes = label.encode()
        version_bytes = version.encode()
        dependencies.extend(
            struct.pack(
                "<HHHH32s",
                kind,
                0,
                len(label_bytes),
                len(version_bytes),
                digest,
            )
        )
        dependencies.extend(label_bytes)
        dependencies.extend(version_bytes)
    metadata = struct.pack(
        "<HHIIHBBQ32sHHI",
        1,
        0,
        0,
        len(entries),
        16,
        20,
        0,
        7,
        config_hash,
        1,
        0,
        0,
    )
    index = struct.pack("<HHIHH", 1, 0, 0, 56, 0)
    return container(
        b"WTWORLD\0",
        7,
        [
            (fourcc(b"META"), metadata),
            (fourcc(b"DEPS"), bytes(dependencies)),
            (fourcc(b"INDX"), index),
        ],
    )


def chunk_page() -> bytes:
    metadata = struct.pack(
        "<HHiiiBBBBHHHBBIQQ",
        1,
        0,
        0,
        0,
        0,
        0,
        255,
        17,
        0,
        19,
        19,
        19,
        1,
        1,
        6859,
        1,
        7,
    )
    samples = struct.pack("<fH", 0.0, 0) * 6859
    return container(
        b"WTCHUNK\0",
        7,
        [
            (fourcc(b"CHDR"), metadata),
            (fourcc(b"DATA"), samples),
        ],
    )


def run_tool(
    executable: Path,
    arguments: list[str],
    expect_success: bool = True,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [str(executable), *arguments],
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
    )
    if expect_success and result.returncode != 0:
        raise RuntimeError(result.stdout + result.stderr)
    if not expect_success and result.returncode == 0:
        raise RuntimeError(f"Tool unexpectedly accepted: {arguments}")
    return result


def test_tools() -> None:
    with tempfile.TemporaryDirectory(prefix="world-transvoxel-m4-") as directory:
        root = Path(directory)
        world_path = root / "legacy.wtworld"
        chunk_path = root / "page.wtchunk"
        corrupt_path = root / "corrupt.wtchunk"
        world_path.write_bytes(legacy_world())
        chunk_bytes = chunk_page()
        chunk_path.write_bytes(chunk_bytes)
        corrupt = bytearray(chunk_bytes)
        corrupt[-1] ^= 0x80
        corrupt_path.write_bytes(corrupt)

        migrated_outputs: list[bytes] = []
        for configuration in ("template_debug", "template_release"):
            executable = native_tool_path(configuration, "wt_storage_tool")
            if not executable.is_file():
                raise RuntimeError(f"Missing storage tool: {executable}")
            world_info = json.loads(
                run_tool(executable, ["inspect", str(world_path)]).stdout
            )
            chunk_info = json.loads(
                run_tool(executable, ["validate", str(chunk_path)]).stdout
            )
            if (
                world_info["type"] != "wtworld"
                or world_info["schema_minor"] != 0
                or world_info["world_revision"] != 0
                or chunk_info["type"] != "wtchunk"
                or chunk_info["samples"] != 6859
            ):
                raise RuntimeError("Storage tool inspection output mismatch.")
            run_tool(
                executable,
                ["validate", str(corrupt_path)],
                expect_success=False,
            )
            migrated_path = root / f"migrated-{configuration}.wtworld"
            migrated_info = json.loads(
                run_tool(
                    executable,
                    ["migrate-world", str(world_path), str(migrated_path)],
                ).stdout
            )
            if (
                migrated_info["schema_minor"] != 1
                or migrated_info["source_revision"] != 7
                or migrated_info["world_revision"] != 0
            ):
                raise RuntimeError("World migration output mismatch.")
            run_tool(executable, ["validate", str(migrated_path)])
            run_tool(
                executable,
                ["migrate-world", str(world_path), str(migrated_path)],
                expect_success=False,
            )
            migrated_outputs.append(migrated_path.read_bytes())
        if migrated_outputs[0] != migrated_outputs[1]:
            raise RuntimeError("Debug/release migration bytes differ.")

        wrapper = REPO_ROOT / "tools" / "wt_storage.py"
        wrapper_result = subprocess.run(
            [sys.executable, str(wrapper), "inspect", str(chunk_path)],
            cwd=REPO_ROOT,
            check=False,
            text=True,
            capture_output=True,
            errors="replace",
        )
        if wrapper_result.returncode != 0:
            raise RuntimeError(wrapper_result.stdout + wrapper_result.stderr)
        if json.loads(wrapper_result.stdout)["type"] != "wtchunk":
            raise RuntimeError("Python storage wrapper output mismatch.")
    print("M4_TOOLS_PASS inspect=2 corruption=1 migration=2 wrapper=1")


if __name__ == "__main__":
    test_tools()
