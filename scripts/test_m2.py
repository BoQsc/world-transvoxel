#!/usr/bin/env python3

from __future__ import annotations

import argparse
import subprocess

from build import build
from test_m1 import test_m1
from wt_script_common import native_test_path


def run_native_test(
    test_name: str,
    configuration: str,
    pass_marker: str,
    hash_prefix: str | None = None,
) -> str | None:
    executable = native_test_path(configuration, test_name)
    if not executable.is_file():
        raise RuntimeError(f"Missing M2 native test executable: {executable}")
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
        raise RuntimeError(f"{test_name} failed for {configuration}.")
    if pass_marker not in combined:
        raise RuntimeError(f"{test_name} pass marker missing for {configuration}.")
    if hash_prefix is None:
        return None
    matching = [
        line for line in combined.splitlines() if line.startswith(hash_prefix)
    ]
    if len(matching) != 1:
        raise RuntimeError(
            f"Expected one {test_name} hash line for {configuration}."
        )
    return matching[0]


def test_m2(skip_build: bool = False, skip_engine_download: bool = False) -> None:
    if not skip_build:
        build("all")

    mesh_hashes = []
    for configuration in ("template_debug", "template_release"):
        run_native_test(
            "test_wt_m2_core",
            configuration,
            "M2_CORE_PASS",
        )
        mesh_hash = run_native_test(
            "test_wt_m2_chunk_mesh",
            configuration,
            "M2_CHUNK_MESH_PASS",
            "M2_MESH_HASH ",
        )
        mesh_hashes.append(mesh_hash)

    if mesh_hashes[0] != mesh_hashes[1]:
        raise RuntimeError(f"M2 debug/release hashes differ: {mesh_hashes}")

    test_m1(skip_build=True, skip_engine_download=skip_engine_download)
    print("M2 validation passed with bounded scheduling, closed seams, and matching hashes.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the complete M2 validation suite.")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    arguments = parser.parse_args()
    test_m2(arguments.skip_build, arguments.skip_engine_download)


if __name__ == "__main__":
    main()
