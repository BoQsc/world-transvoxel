#!/usr/bin/env python3

from __future__ import annotations

import json
import math
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from wt_script_common import REPO_ROOT, native_tool_path


EXPECTED_RELEASE_COMPAT_WORLD_SHA256 = (
    "9ab9d7c9dba2ef01717d9cf5bd952bb6c21451d668f0502d3cbfa88e1204762c"
)


def write_volume(
    density_path: Path,
    material_path: Path,
    *,
    invalid_density: bool = False,
) -> None:
    origin = (-1, -1, -1)
    dimensions = (35, 19, 19)
    densities = bytearray()
    materials = bytearray()
    for z_index in range(dimensions[2]):
        z = origin[2] + z_index
        for y_index in range(dimensions[1]):
            y = origin[1] + y_index
            for x_index in range(dimensions[0]):
                x = origin[0] + x_index
                density = float(x + y + z)
                if invalid_density and (
                    x_index == dimensions[0] - 1
                    and y_index == dimensions[1] - 1
                    and z_index == dimensions[2] - 1
                ):
                    density = math.nan
                densities.extend(struct.pack("<f", density))
                materials.extend(struct.pack("<H", (x - y + z) & 0xFFFF))
    density_path.write_bytes(densities)
    material_path.write_bytes(materials)


def run_baker(
    density: Path,
    materials: Path | None,
    keys: Path,
    output: Path,
    configuration: str,
    *,
    expect_success: bool = True,
    default_material: int = 0,
) -> subprocess.CompletedProcess[str]:
    command = [
        sys.executable,
        str(REPO_ROOT / "tools" / "wt_bake.py"),
        str(density),
        str(keys),
        str(output),
        "--origin",
        "-1",
        "-1",
        "-1",
        "--dimensions",
        "35",
        "19",
        "19",
        "--spacing",
        "1",
        "--source-revision",
        "7",
        "--default-material",
        str(default_material),
        "--configuration",
        configuration,
    ]
    if materials is not None:
        command.extend(["--materials", str(materials)])
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
    )
    if expect_success and result.returncode != 0:
        raise RuntimeError(result.stdout + result.stderr)
    if not expect_success and result.returncode == 0:
        raise RuntimeError("Baker unexpectedly accepted invalid input.")
    return result


def sections(data: bytes) -> dict[int, bytes]:
    directory_size = struct.unpack_from("<Q", data, 40)[0]
    count = directory_size // 68
    output: dict[int, bytes] = {}
    for index in range(count):
        entry = 80 + index * 68
        section_type = struct.unpack_from("<I", data, entry)[0]
        offset, size = struct.unpack_from("<QQ", data, entry + 12)
        output[section_type] = data[offset : offset + size]
    return output


def fourcc(value: bytes) -> int:
    return int.from_bytes(value, "little")


def page_key_and_center(page: bytes) -> tuple[tuple[int, int, int, int], float, int]:
    parsed = sections(page)
    header = parsed[fourcc(b"CHDR")]
    samples = parsed[fourcc(b"DATA")]
    x, y, z = struct.unpack_from("<iii", header, 4)
    lod = header[16]
    index = ((1 * 19 + 1) * 19 + 1)
    density, material = struct.unpack_from("<fH", samples, index * 6)
    return (x, y, z, lod), density, material


def directory_bytes(path: Path) -> dict[str, bytes]:
    return {
        item.name: item.read_bytes()
        for item in sorted(path.iterdir())
        if item.is_file()
    }


def test_baker() -> None:
    with tempfile.TemporaryDirectory(prefix="world-transvoxel-bake-") as directory:
        root = Path(directory)
        density = root / "density.f32"
        materials = root / "materials.u16"
        keys = root / "keys.txt"
        write_volume(density, materials)
        keys.write_text("1 0 0 0\n0 0 0 0\n", encoding="utf-8")

        outputs: list[dict[str, bytes]] = []
        for configuration in ("template_debug", "template_release"):
            output = root / configuration
            result = run_baker(
                density,
                materials,
                keys,
                output,
                configuration,
            )
            bake_info = json.loads(result.stdout)
            if (
                bake_info["type"] != "wtbake"
                or bake_info["pages"] != 2
                or bake_info.get("bounded") is not True
                or bake_info.get("peak_page_payloads") != 1
                or bake_info.get("source_cache_bytes") != 196_608
                or bake_info.get("world_sha256")
                != EXPECTED_RELEASE_COMPAT_WORLD_SHA256
            ):
                raise RuntimeError("Baker JSON output mismatch.")
            files = directory_bytes(output)
            if "world.wtworld" not in files or len(files) != 3:
                raise RuntimeError("Baker artifact set mismatch.")
            storage_tool = native_tool_path(configuration, "wt_storage_tool")
            world_info = json.loads(
                subprocess.run(
                    [str(storage_tool), "validate", str(output / "world.wtworld")],
                    check=True,
                    text=True,
                    capture_output=True,
                ).stdout
            )
            if (
                world_info["pages"] != 2
                or world_info["source_revision"] != 7
                or world_info["world_revision"] != 0
            ):
                raise RuntimeError("Baked world metadata mismatch.")
            page_results = [
                page_key_and_center(data)
                for name, data in files.items()
                if name.endswith(".wtchunk")
            ]
            page_results.sort()
            if [result[0] for result in page_results] != [
                (0, 0, 0, 0),
                (1, 0, 0, 0),
            ]:
                raise RuntimeError("Baked chunk keys mismatch.")
            if page_results[0][1:] != (0.0, 0):
                raise RuntimeError("Dense source center sample mismatch.")
            outputs.append(files)
        if outputs[0] != outputs[1]:
            raise RuntimeError("Debug/release bake artifacts differ.")

        default_keys = root / "default-keys.txt"
        default_keys.write_text("0 0 0 0\n", encoding="utf-8")
        default_output = root / "default-material-output"
        run_baker(
            density,
            None,
            default_keys,
            default_output,
            "template_release",
            default_material=77,
        )
        default_page = next(default_output.glob("*.wtchunk")).read_bytes()
        if page_key_and_center(default_page)[2] != 77:
            raise RuntimeError("Default material bake mismatch.")

        invalid_density = root / "invalid-density.f32"
        write_volume(invalid_density, root / "unused-materials.u16", invalid_density=True)
        invalid_output = root / "invalid-output"
        run_baker(
            invalid_density,
            materials,
            keys,
            invalid_output,
            "template_release",
            expect_success=False,
        )
        if invalid_output.exists() or invalid_output.with_name(
            invalid_output.name + ".tmp"
        ).exists():
            raise RuntimeError("Failed bake left output artifacts.")

        uncovered_keys = root / "uncovered-keys.txt"
        uncovered_keys.write_text("0 0 0 0\n2 0 0 0\n", encoding="utf-8")
        uncovered_output = root / "uncovered-output"
        run_baker(
            density,
            materials,
            uncovered_keys,
            uncovered_output,
            "template_release",
            expect_success=False,
        )
        if uncovered_output.exists():
            raise RuntimeError("Uncovered bake left an output directory.")
    print(
        "M4_BAKE_TOOL_PASS pages=2 cross_build=1 bounded=1 "
        "peak_page_payloads=1 source_cache_bytes=196608 "
        "default_material=1 failure_cases=2 "
        f"world_sha256={EXPECTED_RELEASE_COMPAT_WORLD_SHA256}"
    )


if __name__ == "__main__":
    test_baker()
