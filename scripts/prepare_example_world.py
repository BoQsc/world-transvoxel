#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

from wt_script_common import (
    REPO_ROOT,
    native_tool_path,
    output,
    remove_tree,
    require_supported_python,
    run,
)


SOURCE_ROOT = REPO_ROOT / "build" / "world-transvoxel-example-source"
OUTPUT_ROOT = REPO_ROOT / "build" / "world-transvoxel-example"
ORIGIN = (-2, -2, -2)
DIMENSIONS = (133, 37, 37)
SOURCE_REVISION = 8001
EXPECTED_WORLD_SHA256 = (
    "e44d8955cbe64ce8d606d3a906ba68805014b02ec16edc38f58dad7c6062336a"
)


def write_plane_volume() -> None:
    SOURCE_ROOT.mkdir(parents=True, exist_ok=True)
    densities = bytearray()
    materials = bytearray()
    for _z_index in range(DIMENSIONS[2]):
        for y_index in range(DIMENSIONS[1]):
            y = ORIGIN[1] + y_index
            density = float(y) - 8.25
            for _x_index in range(DIMENSIONS[0]):
                densities.extend(struct.pack("<f", density))
                materials.extend(struct.pack("<H", 7))
    (SOURCE_ROOT / "density.f32").write_bytes(densities)
    (SOURCE_ROOT / "materials.u16").write_bytes(materials)


def write_hierarchical_keys() -> None:
    keys: list[tuple[int, int, int, int]] = []
    for z in range(2):
        for y in range(2):
            for x in range(6):
                keys.append((x, y, z, 0))
    for x in range(4):
        keys.append((x, 0, 0, 1))
    text = "".join(f"{x} {y} {z} {lod}\n" for x, y, z, lod in keys)
    (SOURCE_ROOT / "keys.txt").write_text(text, encoding="utf-8")


def prepare(configuration: str) -> None:
    remove_tree(SOURCE_ROOT, REPO_ROOT)
    remove_tree(OUTPUT_ROOT, REPO_ROOT)
    write_plane_volume()
    write_hierarchical_keys()
    run(
        [
            sys.executable,
            REPO_ROOT / "tools" / "wt_bake.py",
            SOURCE_ROOT / "density.f32",
            SOURCE_ROOT / "keys.txt",
            OUTPUT_ROOT,
            "--materials",
            SOURCE_ROOT / "materials.u16",
            "--origin",
            *[str(value) for value in ORIGIN],
            "--dimensions",
            *[str(value) for value in DIMENSIONS],
            "--spacing",
            "1",
            "--source-revision",
            str(SOURCE_REVISION),
            "--configuration",
            configuration,
        ]
    )
    world_path = OUTPUT_ROOT / "world.wtworld"
    storage_tool = native_tool_path(configuration, "wt_storage_tool")
    metadata = json.loads(output([storage_tool, "validate", world_path]))
    if (
        metadata.get("schema_major") != 1
        or metadata.get("schema_minor") != 1
        or metadata.get("pages") != 28
        or metadata.get("dependencies") != 8
        or metadata.get("source_revision") != SOURCE_REVISION
        or metadata.get("world_revision") != 0
        or metadata.get("sha256") != EXPECTED_WORLD_SHA256
    ):
        raise RuntimeError(f"Example world metadata mismatch: {metadata}")
    print(
        "PRODUCTION_EXAMPLE_FIXTURE_PASS "
        f"path={world_path} pages=28 lods=2 hash={EXPECTED_WORLD_SHA256}"
    )


def main() -> None:
    require_supported_python()
    parser = argparse.ArgumentParser(
        description="Prepare the deterministic World Transvoxel example world."
    )
    parser.add_argument(
        "--configuration",
        choices=("template_debug", "template_release"),
        default="template_release",
    )
    arguments = parser.parse_args()
    prepare(arguments.configuration)


if __name__ == "__main__":
    main()
