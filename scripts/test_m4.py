#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import subprocess
import sys

from build import build
from download_test_engines import engine_executable, engine_specs
from test_m3 import test_m3
from wt_script_common import REPO_ROOT, native_test_path


EXPECTED_BAKE_HASH = "5f01045e4846d377341a72fff5acb84f80d1b8761fe33a89b3b835c6e1a5ef3f"
EXPECTED_WORLD_HASH = "7fd6b604222c4eee8092d8dea6f52c04a0e10435c3a76f55839f97cf24fc2b16"
EXPECTED_EDIT_HASH = "791614cdc4f30d9d346c7ca11ae520d3b52c6b3a8dc804db12bd6a51cc544301"
EXPECTED_SPATIAL_HASH = "dd58c70452ae48e8e32d582d769c13ccfe235b64d612aba668e4ad15d89ef513"
EXPECTED_JOURNAL_HASH = "df6179fa610be63f60aee9c85af340b81f4a6e785ced4e18d428830e605de25a"
EXPECTED_APPLY_HASH = "79f6ee15201acb6862928f361db1f8900469a56cac48e91cb3cfdbb702938daa"
EXPECTED_COMPACTION_HASH = "b4e61fad7fa7281567771d0d698a7234b402aae4d70d49deae3a3b50b5fe02f7"


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
    output = run_native_test(
        configuration,
        "test_wt_m4_compaction",
        "M4_COMPACTION_PASS",
    )
    match = re.search(r"M4_COMPACTION_HASH ([0-9a-f]{64})", output)
    if match is None or match.group(1) != EXPECTED_COMPACTION_HASH:
        actual = "missing" if match is None else match.group(1)
        raise RuntimeError(
            f"M4 compaction hash mismatch for {configuration}: {actual}"
        )


def run_editor_bake_tests() -> None:
    for spec in engine_specs():
        engine = engine_executable(spec)
        result = subprocess.run(
            [
                str(engine),
                "--headless",
                "--path",
                str(REPO_ROOT),
                "--script",
                "res://tests/godot/m4_editor_bake_test.gd",
            ],
            cwd=REPO_ROOT,
            check=False,
            text=True,
            capture_output=True,
            errors="replace",
        )
        combined = result.stdout + result.stderr
        print(combined, end="" if combined.endswith("\n") else "\n")
        if result.returncode != 0 or "M4_EDITOR_BAKE_PASS" not in combined:
            raise RuntimeError(
                f"M4 editor bake scaffolding failed with Godot {spec.version}."
            )


def test_m4(skip_build: bool = False, skip_engine_download: bool = False) -> None:
    if not skip_build:
        build("all")
    for configuration in ("template_debug", "template_release"):
        run_m4_tests(configuration)
    subprocess.run(
        [sys.executable, REPO_ROOT / "scripts" / "test_m4_tools.py"],
        cwd=REPO_ROOT,
        check=True,
    )
    subprocess.run(
        [sys.executable, REPO_ROOT / "scripts" / "test_m4_bake_tool.py"],
        cwd=REPO_ROOT,
        check=True,
    )
    subprocess.run(
        [sys.executable, REPO_ROOT / "tools" / "benchmark_m4_codec.py"],
        cwd=REPO_ROOT,
        check=True,
    )
    run_editor_bake_tests()
    test_m3(skip_build=True, skip_engine_download=skip_engine_download)
    print("M4 validation passed with storage, baking, editing, tooling, migration, and the complete M3 suite.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the current M4 validation suite.")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    arguments = parser.parse_args()
    test_m4(arguments.skip_build, arguments.skip_engine_download)


if __name__ == "__main__":
    main()
