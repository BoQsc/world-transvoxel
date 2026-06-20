#!/usr/bin/env python3

from __future__ import annotations

import argparse
import subprocess

from build import build
from test_m3 import test_m3
from wt_script_common import native_test_path


def run_storage_test(configuration: str) -> None:
    executable = native_test_path(configuration, "test_wt_m4_storage")
    if not executable.is_file():
        raise RuntimeError(f"Missing M4 storage test executable: {executable}")
    result = subprocess.run(
        [str(executable)],
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
    )
    combined = result.stdout + result.stderr
    print(combined, end="" if combined.endswith("\n") else "\n")
    if result.returncode != 0 or "M4_STORAGE_PASS" not in combined:
        raise RuntimeError(f"M4 storage contract failed for {configuration}.")


def test_m4(skip_build: bool = False, skip_engine_download: bool = False) -> None:
    if not skip_build:
        build("all")
    for configuration in ("template_debug", "template_release"):
        run_storage_test(configuration)
    test_m3(skip_build=True, skip_engine_download=skip_engine_download)
    print("M4 storage foundation passed with the complete M3 suite.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the current M4 validation suite.")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    arguments = parser.parse_args()
    test_m4(arguments.skip_build, arguments.skip_engine_download)


if __name__ == "__main__":
    main()
