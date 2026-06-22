#!/usr/bin/env python3

from __future__ import annotations

import argparse
import platform
import subprocess
import sys
from pathlib import Path


ADDON_ROOT = Path(__file__).resolve().parents[1]


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


def tool_path() -> Path:
    system, architecture, suffix = host_identity()
    for configuration in ("template_release", "template_debug"):
        packaged = ADDON_ROOT / "bin" / (
            f"wt_storage_tool.{system}.{configuration}.{architecture}{suffix}"
        )
        if packaged.is_file():
            return packaged
        repository = ADDON_ROOT.parents[1] / "build" / "tools" / (
            f"wt_storage_tool.{configuration}.{architecture}{suffix}"
        )
        if repository.is_file():
            return repository
    raise RuntimeError(
        "The native storage tool is missing. Install a complete release or run "
        "python scripts/build.py from a source checkout."
    )


def run_tool(arguments: list[str]) -> None:
    result = subprocess.run([str(tool_path()), *arguments], check=False)
    if result.returncode != 0:
        raise RuntimeError(
            f"wt_storage_tool failed with exit code {result.returncode}."
        )


def main() -> None:
    require_supported_python()
    parser = argparse.ArgumentParser(
        description="Inspect, validate, and migrate World Transvoxel storage."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("inspect", "validate"):
        subparser = subparsers.add_parser(command)
        subparser.add_argument("path", type=Path)
    migrate = subparsers.add_parser("migrate-world")
    migrate.add_argument("input", type=Path)
    migrate.add_argument("output", type=Path)
    arguments = parser.parse_args()
    if arguments.command in {"inspect", "validate"}:
        run_tool([arguments.command, str(arguments.path.resolve())])
    else:
        run_tool(
            [
                "migrate-world",
                str(arguments.input.resolve()),
                str(arguments.output.resolve()),
            ]
        )


if __name__ == "__main__":
    main()
