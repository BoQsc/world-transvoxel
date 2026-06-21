#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import subprocess

from build import build
from test_m4 import test_m4
from wt_script_common import native_test_path


EXPECTED_ASYNC_STORAGE_HASH = (
    "96ba7123c6b86fe9e2f07aa17f27553f58db92cca6427a069eced98de1471402"
)


def run_async_storage_test(configuration: str) -> None:
    executable = native_test_path(configuration, "test_wt_m5_async_storage")
    if not executable.is_file():
        raise RuntimeError(f"Missing M5 test executable: {executable}")
    result = subprocess.run(
        [str(executable)],
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
    )
    combined = result.stdout + result.stderr
    print(combined, end="" if combined.endswith("\n") else "\n")
    match = re.search(r"M5_ASYNC_STORAGE_HASH ([0-9a-f]{64})", combined)
    if (
        result.returncode != 0
        or "M5_ASYNC_STORAGE_PASS" not in combined
        or match is None
        or match.group(1) != EXPECTED_ASYNC_STORAGE_HASH
    ):
        actual = "missing" if match is None else match.group(1)
        raise RuntimeError(
            f"M5 asynchronous storage contract failed for "
            f"{configuration}: {actual}"
        )


def test_m5(
    skip_build: bool = False,
    skip_engine_download: bool = False,
) -> None:
    if not skip_build:
        build("all")
    for configuration in ("template_debug", "template_release"):
        run_async_storage_test(configuration)
    test_m4(skip_build=True, skip_engine_download=skip_engine_download)
    print(
        "M5 asynchronous storage foundation passed with the complete M4 suite."
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run the active M5 validation suite."
    )
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    arguments = parser.parse_args()
    test_m5(arguments.skip_build, arguments.skip_engine_download)


if __name__ == "__main__":
    main()
