#!/usr/bin/env python3

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

from build import build
from download_test_engines import engine_executable, engine_specs
from test_m2 import test_m2
from wt_script_common import REPO_ROOT, addon_binary_path, native_test_path


ARTIFACT_ROOT = REPO_ROOT / "artifacts" / "m3"


def run_application_test(configuration: str) -> None:
    executable = native_test_path(configuration, "test_wt_m3_application")
    if not executable.is_file():
        raise RuntimeError(f"Missing M3 native test executable: {executable}")
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
        raise RuntimeError(f"M3 application contract failed for {configuration}.")
    if "M3_APPLICATION_PASS" not in combined:
        raise RuntimeError(f"M3 pass marker missing for {configuration}.")


def run_godot_integration(engine: Path, name: str) -> None:
    ARTIFACT_ROOT.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [
            str(engine),
            "--headless",
            "--path",
            str(REPO_ROOT),
            "--script",
            "res://tests/godot/m3_integration_test.gd",
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
    if result.returncode != 0 or "M3_GODOT_INTEGRATION_PASS" not in combined:
        raise RuntimeError(f"M3 Godot integration failed for {name}.")


def run_godot_matrix() -> None:
    engines = [(spec, engine_executable(spec)) for spec in engine_specs()]
    for spec, engine in engines:
        if not engine.is_file():
            raise RuntimeError(f"Required test engine is missing: {engine}")
        run_godot_integration(engine, f"{spec.version}-debug-integration")

    debug_binary = addon_binary_path("template_debug")
    release_binary = addon_binary_path("template_release")
    backup = ARTIFACT_ROOT / f"{debug_binary.name}.backup"
    shutil.copy2(debug_binary, backup)
    try:
        shutil.copy2(release_binary, debug_binary)
        for spec, engine in engines:
            run_godot_integration(engine, f"{spec.version}-release-integration")
    finally:
        shutil.copy2(backup, debug_binary)


def test_m3(skip_build: bool = False, skip_engine_download: bool = False) -> None:
    if not skip_build:
        build("all")
    for configuration in ("template_debug", "template_release"):
        run_application_test(configuration)
    test_m2(skip_build=True, skip_engine_download=skip_engine_download)
    run_godot_matrix()
    print("M3 validation passed with native contracts and the full Godot resource matrix.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the current M3 validation suite.")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    arguments = parser.parse_args()
    test_m3(arguments.skip_build, arguments.skip_engine_download)


if __name__ == "__main__":
    main()
