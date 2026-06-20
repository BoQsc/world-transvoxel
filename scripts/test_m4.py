#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import subprocess

from build import build
from test_m3 import test_m3
from wt_script_common import native_test_path


EXPECTED_BAKE_HASH = "7ed6975c20b67762bd00016b4bebd982b6aafcd4766dc3c0e6bbffaf94dfe5ce"
EXPECTED_WORLD_HASH = "02e209f526c176148bdbcdf40f06ec43747c7b11adc02f6377e64e56e28c3311"
EXPECTED_EDIT_HASH = "b8d28a739463c3e43a20d14f9d0496d3041c8e667e77f1e5f029256855a2b26d"
EXPECTED_SPATIAL_HASH = "dd58c70452ae48e8e32d582d769c13ccfe235b64d612aba668e4ad15d89ef513"
EXPECTED_JOURNAL_HASH = "82ba948c7f37e5812e5fc40331cf7b07c2fbad58a63903f484448d9dcf71de36"
EXPECTED_APPLY_HASH = "86361ec1918d415539e73091c7a9710af9bfecd21a66e3ed5f5e48a3266536df"


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
    output = run_native_test(configuration, "test_wt_m4_world", "M4_WORLD_PASS")
    match = re.search(r"M4_WORLD_HASH ([0-9a-f]{64})", output)
    if match is None or match.group(1) != EXPECTED_WORLD_HASH:
        actual = "missing" if match is None else match.group(1)
        raise RuntimeError(
            f"M4 world hash mismatch for {configuration}: {actual}"
        )
    output = run_native_test(configuration, "test_wt_m4_edit", "M4_EDIT_PASS")
    match = re.search(r"M4_EDIT_HASH ([0-9a-f]{64})", output)
    if match is None or match.group(1) != EXPECTED_EDIT_HASH:
        actual = "missing" if match is None else match.group(1)
        raise RuntimeError(
            f"M4 edit hash mismatch for {configuration}: {actual}"
        )
    output = run_native_test(
        configuration,
        "test_wt_m4_spatial",
        "M4_SPATIAL_PASS",
    )
    match = re.search(r"M4_SPATIAL_HASH ([0-9a-f]{64})", output)
    if match is None or match.group(1) != EXPECTED_SPATIAL_HASH:
        actual = "missing" if match is None else match.group(1)
        raise RuntimeError(
            f"M4 spatial hash mismatch for {configuration}: {actual}"
        )
    output = run_native_test(
        configuration,
        "test_wt_m4_journal",
        "M4_JOURNAL_PASS",
    )
    match = re.search(r"M4_JOURNAL_HASH ([0-9a-f]{64})", output)
    if match is None or match.group(1) != EXPECTED_JOURNAL_HASH:
        actual = "missing" if match is None else match.group(1)
        raise RuntimeError(
            f"M4 journal hash mismatch for {configuration}: {actual}"
        )
    output = run_native_test(
        configuration,
        "test_wt_m4_apply",
        "M4_APPLY_PASS",
    )
    match = re.search(r"M4_APPLY_HASH ([0-9a-f]{64})", output)
    if match is None or match.group(1) != EXPECTED_APPLY_HASH:
        actual = "missing" if match is None else match.group(1)
        raise RuntimeError(
            f"M4 apply hash mismatch for {configuration}: {actual}"
        )


def test_m4(skip_build: bool = False, skip_engine_download: bool = False) -> None:
    if not skip_build:
        build("all")
    for configuration in ("template_debug", "template_release"):
        run_m4_tests(configuration)
    test_m3(skip_build=True, skip_engine_download=skip_engine_download)
    print("M4 storage, baking, indexing, and authoritative edit replay passed with the complete M3 suite.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the current M4 validation suite.")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    arguments = parser.parse_args()
    test_m4(arguments.skip_build, arguments.skip_engine_download)


if __name__ == "__main__":
    main()
