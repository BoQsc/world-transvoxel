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


EXPECTED_BAKE_HASH = "8945e3394a7ee1bcf4f7eec0a07da792ad8296ab35ef8f3413e11cc1b1fcf5bf"
EXPECTED_WORLD_HASH = "b7b4493e4b0fc5275b283117abbfb0c4fb57f8fc53729e047b895f30155bcf17"
EXPECTED_EDIT_HASH = "8ecb3f387478877235fef366805b9e72b6cdb927126e5fa9b545f5a957779f60"
EXPECTED_SPATIAL_HASH = "dd58c70452ae48e8e32d582d769c13ccfe235b64d612aba668e4ad15d89ef513"
EXPECTED_JOURNAL_HASH = "7f448a317d6cf22038b276f29700822752abe17c8f56b83a86018fe7ebd5d063"
EXPECTED_APPLY_HASH = "718299711f482dec5c606b31fefa61e6576d73f4226c0b84c5059e70031eded1"
EXPECTED_COMPACTION_HASH = "da36002c73751648e52ab845dfa8b7b33923c8284f8d7f8cf70bf276e9961e0e"


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
