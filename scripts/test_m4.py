#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import subprocess

from build import build
from test_m3 import test_m3
from wt_script_common import native_test_path


EXPECTED_BAKE_HASH = "7ed6975c20b67762bd00016b4bebd982b6aafcd4766dc3c0e6bbffaf94dfe5ce"


def run_native_test(configuration: str, test_name: str, pass_marker: str) -> str:
    executable = native_test_path(configuration, test_name)
    if not executable.is_file():
        raise RuntimeError(f"Missing M4 test executable: {executable}")
    result = subprocess.run(
        [str(executable)],
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
    )
    combined = result.stdout + result.stderr
    print(combined, end="" if combined.endswith("\n") else "\n")
    if result.returncode != 0 or pass_marker not in combined:
        raise RuntimeError(
            f"{test_name} contract failed for {configuration}."
        )
    return combined


def run_m4_tests(configuration: str) -> None:
    run_native_test(configuration, "test_wt_m4_storage", "M4_STORAGE_PASS")
    output = run_native_test(configuration, "test_wt_m4_bake", "M4_BAKE_PASS")
    match = re.search(r"M4_BAKE_HASH ([0-9a-f]{64})", output)
    if match is None or match.group(1) != EXPECTED_BAKE_HASH:
        actual = "missing" if match is None else match.group(1)
        raise RuntimeError(
            f"M4 bake hash mismatch for {configuration}: {actual}"
        )


def test_m4(skip_build: bool = False, skip_engine_download: bool = False) -> None:
    if not skip_build:
        build("all")
    for configuration in ("template_debug", "template_release"):
        run_m4_tests(configuration)
    test_m3(skip_build=True, skip_engine_download=skip_engine_download)
    print("M4 chunk storage and baking passed with the complete M3 suite.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the current M4 validation suite.")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    arguments = parser.parse_args()
    test_m4(arguments.skip_build, arguments.skip_engine_download)


if __name__ == "__main__":
    main()
