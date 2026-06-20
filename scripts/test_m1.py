#!/usr/bin/env python3

from __future__ import annotations

import argparse
import subprocess

from build import build
from test_m0 import test_m0
from wt_script_common import native_test_path


def test_m1(skip_build: bool = False, skip_engine_download: bool = False) -> None:
    if not skip_build:
        build("all")

    hash_lines = []
    for configuration in ("template_debug", "template_release"):
        executable = native_test_path(configuration)
        if not executable.is_file():
            raise RuntimeError(f"Missing M1 native test executable: {executable}")
        result = subprocess.run(
            [str(executable)],
            check=False,
            text=True,
            capture_output=True,
            errors="replace",
        )
        combined = result.stdout + result.stderr
        print(combined, end="" if combined.endswith("\n") else "\n")
        if result.returncode != 0:
            raise RuntimeError(f"M1 native contract failed for {configuration}.")
        if "M1_CELL_BACKEND_PASS" not in combined:
            raise RuntimeError(f"M1 pass marker missing for {configuration}.")
        matching = [line for line in combined.splitlines() if line.startswith("M1_HASHES ")]
        if len(matching) != 1:
            raise RuntimeError(f"Expected one M1 hash line for {configuration}.")
        hash_lines.append(matching[0])

    if hash_lines[0] != hash_lines[1]:
        raise RuntimeError(f"M1 debug/release hashes differ: {hash_lines}")

    test_m0(skip_build=True, skip_engine_download=skip_engine_download)
    print("M1 validation passed with matching hashes and Godot compatibility.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the complete M1 validation suite.")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    arguments = parser.parse_args()
    test_m1(arguments.skip_build, arguments.skip_engine_download)


if __name__ == "__main__":
    main()
