#!/usr/bin/env python3

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Sequence

from build import build
from download_test_engines import download_test_engines, engine_executable, engine_specs
from wt_script_common import REPO_ROOT, addon_binary_path, run


ARTIFACT_ROOT = REPO_ROOT / "artifacts" / "m0"


def run_godot(engine: Path, arguments: Sequence[str], name: str) -> str:
    ARTIFACT_ROOT.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [str(engine), *arguments],
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
    if result.returncode != 0:
        raise RuntimeError(f"Godot command {name} failed with code {result.returncode}.")
    return combined


def test_m0(skip_build: bool = False, skip_engine_download: bool = False) -> None:
    if not skip_build:
        build("all")
    run([sys.executable, REPO_ROOT / "tools" / "validate_repository.py", "--require-binaries"])
    if not skip_engine_download:
        download_test_engines()

    engines = [(spec, engine_executable(spec)) for spec in engine_specs()]
    for spec, engine in engines:
        if not engine.is_file():
            raise RuntimeError(f"Required test engine is missing: {engine}")
        version = run_godot(
            engine,
            ["--headless", "--version"],
            f"{spec.version}-version",
        ).strip()
        print(f"Load-testing with Godot {version}")
        run_godot(
            engine,
            ["--headless", "--editor", "--path", str(REPO_ROOT), "--quit"],
            f"{spec.version}-editor-load",
        )
        runtime_output = run_godot(
            engine,
            [
                "--headless",
                "--path",
                str(REPO_ROOT),
                "--script",
                "res://tests/godot/addon_load_test.gd",
            ],
            f"{spec.version}-runtime-load",
        )
        if "ADDON_LOAD_TEST_PASS" not in runtime_output:
            raise RuntimeError(f"Addon pass marker missing with Godot {version}.")

    debug_binary = addon_binary_path("template_debug")
    release_binary = addon_binary_path("template_release")
    backup = ARTIFACT_ROOT / f"{debug_binary.name}.backup"
    shutil.copy2(debug_binary, backup)
    try:
        shutil.copy2(release_binary, debug_binary)
        for spec, engine in engines:
            release_output = run_godot(
                engine,
                [
                    "--headless",
                    "--path",
                    str(REPO_ROOT),
                    "--script",
                    "res://tests/godot/addon_load_test.gd",
                ],
                f"{spec.version}-template-release-load",
            )
            if "ADDON_LOAD_TEST_PASS" not in release_output:
                raise RuntimeError(
                    f"Addon release pass marker missing with Godot {spec.version}."
                )
    finally:
        shutil.copy2(backup, debug_binary)

    print("M0 compatibility baseline passed on all configured engines.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the M0 addon compatibility baseline.")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    arguments = parser.parse_args()
    test_m0(arguments.skip_build, arguments.skip_engine_download)


if __name__ == "__main__":
    main()
