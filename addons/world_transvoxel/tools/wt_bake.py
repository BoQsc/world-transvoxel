#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import subprocess
import sys
from pathlib import Path


ADDON_ROOT = Path(__file__).resolve().parents[1]
TRANSVOXEL_CPP_SHA256 = (
    "83a5511346b54c42e4e66dec916d3971c92f4fbda1c7878cbad5901a820dcab4"
)
GODOT_SUPPORTED_MATRIX = "4.6.3+4.7"
GODOT_CPP_REVISION = "e83fd0904c13356ed1d4c3d09f8bb9132bdc6b77"
ZIG_VERSION = "0.16.0"


def require_supported_python() -> None:
    if sys.version_info < (3, 11):
        raise RuntimeError("World Transvoxel tooling requires Python 3.11 or newer.")


def host_identity() -> tuple[str, str, str]:
    machine = platform.machine().lower()
    if machine in {"amd64", "x86_64"}:
        architecture = "x86_64"
    elif machine in {"arm64", "aarch64"}:
        architecture = "arm64"
    else:
        raise RuntimeError(f"Unsupported host architecture: {platform.machine()}")
    if sys.platform == "win32":
        return "windows", architecture, ".exe"
    if sys.platform.startswith("linux"):
        return "linux", architecture, ""
    if sys.platform == "darwin":
        return "macos", architecture, ""
    raise RuntimeError(f"Unsupported host operating system: {sys.platform}")


def native_tool_path(configuration: str) -> Path:
    system, architecture, suffix = host_identity()
    filename = f"wt_bake_tool.{configuration}.{architecture}{suffix}"
    packaged = ADDON_ROOT / "bin" / (
        f"wt_bake_tool.{system}.{configuration}.{architecture}{suffix}"
    )
    if packaged.is_file():
        return packaged
    repository = ADDON_ROOT.parents[1] / "build" / "tools" / filename
    if repository.is_file():
        return repository
    raise RuntimeError(
        "The native bake tool is missing. Install a complete release or run "
        "python scripts/build.py from a source checkout."
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def addon_version() -> str:
    header = (ADDON_ROOT / "src" / "core" / "wt_version.h").read_text(
        encoding="utf-8"
    )
    match = re.search(r'kAddonVersion\s*=\s*"([^"]+)"', header)
    if match is None:
        raise RuntimeError("Could not read kAddonVersion.")
    return match.group(1)


def source_hashes(arguments: argparse.Namespace) -> tuple[str, str | None]:
    return (
        sha256(arguments.density),
        sha256(arguments.materials) if arguments.materials is not None else None,
    )


def configuration_hash(
    arguments: argparse.Namespace,
    density_hash: str,
    material_hash: str | None,
) -> str:
    payload = {
        "schema": "world-transvoxel.dense-bake.v1",
        "density_sha256": density_hash,
        "materials_sha256": material_hash,
        "keys_sha256": sha256(arguments.keys),
        "origin": arguments.origin,
        "dimensions": arguments.dimensions,
        "spacing": arguments.spacing,
        "source_revision": arguments.source_revision,
        "default_material": arguments.default_material,
    }
    encoded = json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    ).encode()
    return hashlib.sha256(encoded).hexdigest()


def main() -> None:
    require_supported_python()
    parser = argparse.ArgumentParser(
        description="Bake dense scalar/material volumes with World Transvoxel."
    )
    parser.add_argument("density", type=Path)
    parser.add_argument("keys", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--materials", type=Path)
    parser.add_argument("--origin", type=int, nargs=3, required=True)
    parser.add_argument("--dimensions", type=int, nargs=3, required=True)
    parser.add_argument("--spacing", type=int, default=1)
    parser.add_argument("--source-revision", type=int, required=True)
    parser.add_argument("--default-material", type=int, default=0)
    parser.add_argument(
        "--configuration",
        choices=("template_debug", "template_release"),
        default="template_release",
    )
    arguments = parser.parse_args()

    for path in (arguments.density, arguments.keys, arguments.materials):
        if path is not None and not path.is_file():
            raise RuntimeError(f"Missing input file: {path}")
    if arguments.output.exists():
        raise RuntimeError(f"Output already exists: {arguments.output}")
    if (
        any(value <= 0 for value in arguments.dimensions)
        or arguments.spacing <= 0
        or arguments.source_revision < 0
        or not 0 <= arguments.default_material <= 65535
    ):
        raise RuntimeError("Invalid dimensions, spacing, revision, or material.")

    density_hash, material_hash = source_hashes(arguments)
    command = [
        str(native_tool_path(arguments.configuration)),
        "dense",
        str(arguments.density.resolve()),
        (
            str(arguments.materials.resolve())
            if arguments.materials is not None
            else "-"
        ),
        str(arguments.keys.resolve()),
        str(arguments.output.resolve()),
        *(str(value) for value in arguments.origin),
        *(str(value) for value in arguments.dimensions),
        str(arguments.spacing),
        str(arguments.source_revision),
        str(arguments.default_material),
        configuration_hash(arguments, density_hash, material_hash),
        density_hash,
        material_hash or "-",
        TRANSVOXEL_CPP_SHA256,
        addon_version(),
        GODOT_SUPPORTED_MATRIX,
        GODOT_CPP_REVISION,
        ZIG_VERSION,
    ]
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(
            f"wt_bake_tool failed with exit code {result.returncode}."
        )


if __name__ == "__main__":
    main()
