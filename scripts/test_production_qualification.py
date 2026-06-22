#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

from build import build
from download_test_engines import (
    download_test_engines,
    engine_executable,
    engine_specs,
)
from test_m5 import test_m5
from test_pq3 import DEFAULT_DURATION_MS as PQ3_DURATION_MS
from test_pq3 import test_pq3
from wt_script_common import REPO_ROOT, addon_binary_path, native_test_path


ARTIFACT_ROOT = REPO_ROOT / "artifacts" / "production-qualification"
EXPECTED_CONFIG_HASH = (
    "c21861342875b96db0bad453f8251d4586994f579e75188be102af1155af0104"
)
EXPECTED_EDIT_JOURNAL_HASH = (
    "f6331a4a71a1c1ddb4bfd2aca61562dd5358710a9888eabd6b1b9308229e64e6"
)
EXPECTED_SNAPSHOT_QUERY_HASH = (
    "b7eb3601b35571ad4c701205be4a4f5b918ab12f142c357abf0c15453f6440f9"
)
EXPECTED_LIFECYCLE_HASH = (
    "ccdb1e1ad000f824ebd4628e640a6c1d95f9d734cc1298f738de3d0c98f3a126"
)
EXPECTED_STREAMING_HASH = (
    "39db05c67fc2f4b8d8beaab2e7da927ae968efb3d75118bcd80c5523116d9b3b"
)
EXPECTED_LOD_STREAMING_HASH = (
    "cf0d7ca3f67013d99d0f909368d179aa6878a0bbd7b57ee409ac5c8994681102"
)
LIFECYCLE_FIXTURE_ROOT = REPO_ROOT / "build" / "production-lifecycle-fixture"


def run_hashed_native(
    configuration: str,
    test_name: str,
    pass_marker: str,
    hash_label: str,
    expected_hash: str,
) -> None:
    executable = native_test_path(configuration, test_name)
    if not executable.is_file():
        raise RuntimeError(f"Missing production native test: {executable}")
    result = subprocess.run(
        [str(executable)],
        cwd=REPO_ROOT,
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
    )
    combined = result.stdout + result.stderr
    print(combined, end="" if combined.endswith("\n") else "\n")
    match = re.search(rf"{hash_label} ([0-9a-f]{{64}})", combined)
    if (
        result.returncode != 0
        or pass_marker not in combined
        or match is None
        or match.group(1) != expected_hash
    ):
        actual = "missing" if match is None else match.group(1)
        raise RuntimeError(
            f"{test_name} failed for {configuration}: {actual}"
        )


def prepare_lifecycle_fixture() -> None:
    if LIFECYCLE_FIXTURE_ROOT.exists():
        shutil.rmtree(LIFECYCLE_FIXTURE_ROOT)
    executable = native_test_path(
        "template_release", "test_wt_production_lifecycle"
    )
    result = subprocess.run(
        [
            str(executable),
            "--write-godot-fixture",
            str(LIFECYCLE_FIXTURE_ROOT),
        ],
        cwd=REPO_ROOT,
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
    )
    combined = result.stdout + result.stderr
    print(combined, end="" if combined.endswith("\n") else "\n")
    if (
        result.returncode != 0
        or "PRODUCTION_LIFECYCLE_FIXTURE_PASS" not in combined
        or "PRODUCTION_STREAMING_FIXTURE_PASS" not in combined
        or "PRODUCTION_TRANSITION_FIXTURE_PASS" not in combined
        or "PRODUCTION_LEGACY_FIXTURE_PASS" not in combined
        or not (LIFECYCLE_FIXTURE_ROOT / "world.wtworld").is_file()
        or not (LIFECYCLE_FIXTURE_ROOT / "streaming.wtworld").is_file()
        or not (LIFECYCLE_FIXTURE_ROOT / "transition.wtworld").is_file()
        or not (LIFECYCLE_FIXTURE_ROOT / "legacy.wtworld").is_file()
    ):
        raise RuntimeError("Production lifecycle fixture generation failed.")


def prepare_example_fixture() -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(REPO_ROOT / "scripts" / "prepare_example_world.py"),
            "--configuration",
            "template_release",
        ],
        cwd=REPO_ROOT,
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
    )
    combined = result.stdout + result.stderr
    print(combined, end="" if combined.endswith("\n") else "\n")
    if (
        result.returncode != 0
        or "PRODUCTION_EXAMPLE_FIXTURE_PASS" not in combined
        or not (
            REPO_ROOT
            / "build"
            / "world-transvoxel-example"
            / "world.wtworld"
        ).is_file()
    ):
        raise RuntimeError("Production example fixture generation failed.")


def run_godot_test(
    engine: Path,
    name: str,
    script: str,
    pass_marker: str,
) -> None:
    ARTIFACT_ROOT.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [
            str(engine),
            "--headless",
            "--path",
            str(REPO_ROOT),
            "--script",
            script,
        ],
        cwd=REPO_ROOT,
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
    )
    (ARTIFACT_ROOT / f"{name}.stdout.txt").write_text(
        result.stdout, encoding="utf-8"
    )
    (ARTIFACT_ROOT / f"{name}.stderr.txt").write_text(
        result.stderr, encoding="utf-8"
    )
    combined = result.stdout + result.stderr
    print(combined, end="" if combined.endswith("\n") else "\n")
    if result.returncode != 0 or pass_marker not in combined:
        raise RuntimeError(f"Production Godot test failed for {name}.")


def run_engine_tests(engine: Path, name: str) -> None:
    run_godot_test(
        engine,
        f"{name}-config",
        "res://tests/godot/production_config_test.gd",
        "PRODUCTION_GODOT_CONFIG_PASS",
    )
    run_godot_test(
        engine,
        f"{name}-lifecycle",
        "res://tests/godot/production_lifecycle_test.gd",
        "PRODUCTION_GODOT_LIFECYCLE_PASS",
    )
    run_godot_test(
        engine,
        f"{name}-streaming",
        "res://tests/godot/production_streaming_test.gd",
        "PRODUCTION_GODOT_STREAMING_PASS",
    )
    run_godot_test(
        engine,
        f"{name}-lod-streaming",
        "res://tests/godot/production_lod_streaming_test.gd",
        "PRODUCTION_GODOT_LOD_STREAMING_PASS",
    )
    run_godot_test(
        engine,
        f"{name}-example",
        "res://tests/godot/production_example_test.gd",
        "PRODUCTION_GODOT_EXAMPLE_PASS",
    )
    run_godot_test(
        engine,
        f"{name}-chunk-query",
        "res://tests/godot/production_chunk_query_test.gd",
        "PRODUCTION_GODOT_CHUNK_QUERY_PASS",
    )
    run_godot_test(
        engine,
        f"{name}-edit-journal",
        "res://tests/godot/production_edit_journal_test.gd",
        "PRODUCTION_GODOT_EDIT_JOURNAL_PASS",
    )
    run_godot_test(
        engine,
        f"{name}-snapshot-query",
        "res://tests/godot/production_snapshot_query_test.gd",
        "PRODUCTION_GODOT_SNAPSHOT_QUERY_PASS",
    )


def run_godot_matrix() -> None:
    engines = [(spec, engine_executable(spec)) for spec in engine_specs()]
    for spec, engine in engines:
        if not engine.is_file():
            raise RuntimeError(f"Required test engine is missing: {engine}")
        run_engine_tests(engine, f"{spec.version}-debug")

    debug_binary = addon_binary_path("template_debug")
    release_binary = addon_binary_path("template_release")
    backup = ARTIFACT_ROOT / f"{debug_binary.name}.backup"
    shutil.copy2(debug_binary, backup)
    try:
        shutil.copy2(release_binary, debug_binary)
        for spec, engine in engines:
            run_engine_tests(engine, f"{spec.version}-release")
    finally:
        shutil.copy2(backup, debug_binary)


def test_production_qualification(
    skip_build: bool = False,
    skip_engine_download: bool = False,
    pq3_duration_ms: int = PQ3_DURATION_MS,
    write_pq3_reference_evidence: bool = False,
) -> None:
    if not skip_build:
        build("all")
    if not skip_engine_download:
        download_test_engines()
    for configuration in ("template_debug", "template_release"):
        run_hashed_native(
            configuration,
            "test_wt_production_config",
            "PRODUCTION_CONFIG_PASS",
            "PRODUCTION_CONFIG_HASH",
            EXPECTED_CONFIG_HASH,
        )
        run_hashed_native(
            configuration,
            "test_wt_production_edit_journal",
            "PRODUCTION_EDIT_JOURNAL_PASS",
            "PRODUCTION_EDIT_JOURNAL_HASH",
            EXPECTED_EDIT_JOURNAL_HASH,
        )
        run_hashed_native(
            configuration,
            "test_wt_production_snapshot_query",
            "PRODUCTION_SNAPSHOT_QUERY_PASS",
            "PRODUCTION_SNAPSHOT_QUERY_HASH",
            EXPECTED_SNAPSHOT_QUERY_HASH,
        )
        run_hashed_native(
            configuration,
            "test_wt_production_lifecycle",
            "PRODUCTION_LIFECYCLE_PASS",
            "PRODUCTION_LIFECYCLE_HASH",
            EXPECTED_LIFECYCLE_HASH,
        )
        run_hashed_native(
            configuration,
            "test_wt_production_streaming",
            "PRODUCTION_STREAMING_PASS",
            "PRODUCTION_STREAMING_HASH",
            EXPECTED_STREAMING_HASH,
        )
        run_hashed_native(
            configuration,
            "test_wt_production_lod_streaming",
            "PRODUCTION_LOD_STREAMING_PASS",
            "PRODUCTION_LOD_STREAMING_HASH",
            EXPECTED_LOD_STREAMING_HASH,
        )
    prepare_lifecycle_fixture()
    prepare_example_fixture()
    run_godot_matrix()
    test_pq3(
        skip_build=True,
        skip_engine_download=True,
        duration_ms=pq3_duration_ms,
        write_reference_evidence=write_pq3_reference_evidence,
        prepare_fixture=False,
    )
    test_m5(skip_build=True, skip_engine_download=skip_engine_download)
    print(
        "Production qualification configuration, lifecycle, balanced multi-LOD "
        "streaming, durable editing/restart replay, root example, and complete "
        "M5 regression suite passed; PQ3 isolated clean-install full-world "
        "soak, compaction, migration, reopen, and shutdown are complete."
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run the active production qualification suite."
    )
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    parser.add_argument(
        "--pq3-duration-ms", type=int, default=PQ3_DURATION_MS
    )
    parser.add_argument(
        "--write-pq3-reference-evidence", action="store_true"
    )
    arguments = parser.parse_args()
    test_production_qualification(
        arguments.skip_build,
        arguments.skip_engine_download,
        arguments.pq3_duration_ms,
        arguments.write_pq3_reference_evidence,
    )


if __name__ == "__main__":
    main()
