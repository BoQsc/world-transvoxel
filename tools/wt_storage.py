#!/usr/bin/env python3

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from wt_script_common import native_tool_path, require_supported_python


def tool_path() -> Path:
    for configuration in ("template_release", "template_debug"):
        candidate = native_tool_path(configuration, "wt_storage_tool")
        if candidate.is_file():
            return candidate
    raise RuntimeError(
        "wt_storage_tool is not built. Run python scripts/build.py first."
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
