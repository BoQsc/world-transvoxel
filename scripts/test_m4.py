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


EXPECTED_BAKE_HASH = "85b71cb2803a1d7c405f20bc287c17c9384487e63f1ed6b61d5c179111756fb3"
EXPECTED_WORLD_HASH = "027931b28de539fa4936d5c394fb91e1757c154e30f6d325ec7ee48b503254db"
EXPECTED_EDIT_HASH = "8ecb3f387478877235fef366805b9e72b6cdb927126e5fa9b545f5a957779f60"
EXPECTED_SPATIAL_HASH = "dd58c70452ae48e8e32d582d769c13ccfe235b64d612aba668e4ad15d89ef513"
EXPECTED_JOURNAL_HASH = "7f448a317d6cf22038b276f29700822752abe17c8f56b83a86018fe7ebd5d063"
EXPECTED_APPLY_HASH = "4218e0b2e7cff2e44cac31e3b5cf44df9c212a029c88be5a22b0c988e7a3d4a1"
EXPECTED_COMPACTION_HASH = "ec53c231579f4786afca3ce354d16165baa0386f3586cd2709a17c07568cbe11"


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
