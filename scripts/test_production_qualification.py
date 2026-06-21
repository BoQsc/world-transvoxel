#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
from pathlib import Path

from build import build
from download_test_engines import (
    download_test_engines,
    engine_executable,
    engine_specs,
)
from test_m5 import test_m5
from wt_script_common import REPO_ROOT, addon_binary_path, native_test_path


ARTIFACT_ROOT = REPO_ROOT / "artifacts" / "production-qualification"
EXPECTED_CONFIG_HASH = (
    "c21861342875b96db0bad453f8251d4586994f579e75188be102af1155af0104"
)


def run_native_config(configuration: str) -> None:
    executable = native_test_path(configuration, "test_wt_production_config")
    if not executable.is_file():
        raise RuntimeError(f"Missing production config test: {executable}")
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
    match = re.search(r"PRODUCTION_CONFIG_HASH ([0-9a-f]{64})", combined)
    if (
        result.returncode != 0
        or "PRODUCTION_CONFIG_PASS" not in combined
        or match is None
        or match.group(1) != EXPECTED_CONFIG_HASH
    ):
        actual = "missing" if match is None else match.group(1)
        raise RuntimeError(
            f"Production config contract failed for {configuration}: {actual}"
        )


def run_godot_config(engine: Path, name: str) -> None:
    ARTIFACT_ROOT.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [
            str(engine),
            "--headless",
            "--path",
            str(REPO_ROOT),
            "--script",
            "res://tests/godot/production_config_test.gd",
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
    if result.returncode != 0 or "PRODUCTION_GODOT_CONFIG_PASS" not in combined:
        raise RuntimeError(f"Production Godot config failed for {name}.")


def run_godot_matrix() -> None:
    engines = [(spec, engine_executable(spec)) for spec in engine_specs()]
    for spec, engine in engines:
        if not engine.is_file():
            raise RuntimeError(f"Required test engine is missing: {engine}")
        run_godot_config(engine, f"{spec.version}-debug-config")

    debug_binary = addon_binary_path("template_debug")
    release_binary = addon_binary_path("template_release")
    backup = ARTIFACT_ROOT / f"{debug_binary.name}.backup"
    shutil.copy2(debug_binary, backup)
    try:
        shutil.copy2(release_binary, debug_binary)
        for spec, engine in engines:
            run_godot_config(engine, f"{spec.version}-release-config")
    finally:
        shutil.copy2(backup, debug_binary)


def test_production_qualification(
    skip_build: bool = False,
    skip_engine_download: bool = False,
) -> None:
    if not skip_build:
        build("all")
    if not skip_engine_download:
        download_test_engines()
    for configuration in ("template_debug", "template_release"):
        run_native_config(configuration)
    run_godot_matrix()
    test_m5(skip_build=True, skip_engine_download=skip_engine_download)
    print(
        "Production qualification configuration foundation passed with the "
        "complete M5 regression suite."
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run the active production qualification suite."
    )
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    arguments = parser.parse_args()
    test_production_qualification(
        arguments.skip_build,
        arguments.skip_engine_download,
    )


if __name__ == "__main__":
    main()
