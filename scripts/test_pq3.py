#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import platform
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
from prepare_example_world import prepare as prepare_example_world
from wt_script_common import (
    REPO_ROOT,
    addon_binary_path,
    host_platform,
    remove_tree,
    require_supported_python,
    sha256,
)


DEFAULT_DURATION_MS = 15000
CLEAN_ROOT = REPO_ROOT / "build" / "pq3-clean-install"
ARTIFACT_ROOT = REPO_ROOT / "artifacts" / "pq3"
REFERENCE_EVIDENCE = (
    REPO_ROOT
    / "docs"
    / "evidence"
    / "pq3_clean_install_soak_windows_x86_64.json"
)
PASS_MARKER = "PRODUCTION_GODOT_FULL_WORLD_SOAK_PASS "
PROJECT_TEXT = """; Generated isolated PQ3 qualification project.
config_version=5

[application]

config/name="World Transvoxel PQ3 Clean Install"

[editor_plugins]

enabled=PackedStringArray("res://addons/world_transvoxel/plugin.cfg")

[rendering]

renderer/rendering_method="gl_compatibility"
renderer/rendering_method.mobile="gl_compatibility"
"""
NOTICE_PATHS = (
    Path("LICENSE"),
    Path("LICENSE_SCOPE.md"),
    Path("LICENSES/MIT-Transvoxel.txt"),
)


def copy_clean_project(
    case_root: Path,
    configuration: str,
    distribution_root: Path,
) -> dict[str, object]:
    remove_tree(case_root, CLEAN_ROOT)
    case_root.mkdir(parents=True)
    source_addon = distribution_root / "addons" / "world_transvoxel"
    target_addon = case_root / "addons" / "world_transvoxel"
    shutil.copytree(
        source_addon,
        target_addon,
        ignore=shutil.ignore_patterns("bin"),
    )
    target_bin = target_addon / "bin"
    target_bin.mkdir()
    debug_binary = distribution_binary_path(
        distribution_root, "template_debug"
    )
    release_binary = distribution_binary_path(
        distribution_root, "template_release"
    )
    if not debug_binary.is_file() or not release_binary.is_file():
        raise RuntimeError("PQ3 requires both addon runtime binaries.")
    shutil.copy2(debug_binary, target_bin / debug_binary.name)
    shutil.copy2(release_binary, target_bin / release_binary.name)
    if configuration == "template_release":
        shutil.copy2(release_binary, target_bin / debug_binary.name)

    for relative in NOTICE_PATHS:
        target = case_root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(distribution_root / relative, target)
    (case_root / "project.godot").write_text(PROJECT_TEXT, encoding="utf-8")
    test_root = case_root / "tests" / "godot"
    test_root.mkdir(parents=True)
    for name in (
        "production_full_world_soak.gd",
        "production_full_world_soak_support.gd",
    ):
        shutil.copy2(REPO_ROOT / "tests" / "godot" / name, test_root / name)
        shutil.copy2(
            REPO_ROOT / "tests" / "godot" / f"{name}.uid",
            test_root / f"{name}.uid",
        )
    fixture_root = REPO_ROOT / "build" / "world-transvoxel-example"
    if not (fixture_root / "world.wtworld").is_file():
        raise RuntimeError("PQ3 baked-world fixture is missing.")
    shutil.copytree(fixture_root, case_root / "world")
    verify_clean_project(case_root, distribution_root)
    return distribution_identity(case_root)


def distribution_binary_path(
    distribution_root: Path,
    configuration: str,
) -> Path:
    return (
        distribution_root
        / "addons"
        / "world_transvoxel"
        / "bin"
        / addon_binary_path(configuration).name
    )


def verify_clean_project(
    case_root: Path,
    distribution_root: Path,
) -> None:
    expected_top_level = {
        "LICENSE",
        "LICENSE_SCOPE.md",
        "LICENSES",
        "addons",
        "project.godot",
        "tests",
        "world",
    }
    actual_top_level = {entry.name for entry in case_root.iterdir()}
    if actual_top_level != expected_top_level:
        raise RuntimeError(
            f"PQ3 clean project boundary mismatch: {sorted(actual_top_level)}"
        )
    forbidden = {
        ".git",
        ".godot",
        ".tools",
        "artifacts",
        "build_profiles",
        "docs",
        "references",
        "scripts",
        "thirdparty",
        "tools",
        "world_transvoxel",
    }
    if actual_top_level & forbidden:
        raise RuntimeError("PQ3 clean project contains repository-only content.")
    for path in case_root.rglob("*"):
        if path.is_symlink():
            raise RuntimeError(f"PQ3 clean project contains a link: {path}")
    if (
        (case_root / "LICENSE").read_bytes()
        != (distribution_root / "LICENSE").read_bytes()
        or (case_root / "LICENSE_SCOPE.md").read_bytes()
        != (distribution_root / "LICENSE_SCOPE.md").read_bytes()
        or (case_root / "LICENSES" / "MIT-Transvoxel.txt").read_bytes()
        != (
            distribution_root / "LICENSES" / "MIT-Transvoxel.txt"
        ).read_bytes()
        or (
            case_root
            / "addons"
            / "world_transvoxel"
            / "thirdparty"
            / "transvoxel_mit"
            / "LICENSE"
        ).read_text(encoding="utf-8").splitlines()
        != (
            distribution_root / "LICENSES" / "MIT-Transvoxel.txt"
        ).read_text(encoding="utf-8").splitlines()
    ):
        raise RuntimeError("PQ3 clean project license notices are incomplete.")


def distribution_identity(case_root: Path) -> dict[str, object]:
    selected: list[Path] = []
    for relative in NOTICE_PATHS:
        selected.append(case_root / relative)
    selected.extend(
        path
        for path in (case_root / "addons" / "world_transvoxel").rglob("*")
        if path.is_file()
    )
    selected.sort(key=lambda path: path.relative_to(case_root).as_posix())
    digest = hashlib.sha256()
    total_bytes = 0
    for path in selected:
        relative = path.relative_to(case_root).as_posix().encode("utf-8")
        contents = path.read_bytes()
        digest.update(len(relative).to_bytes(4, "little"))
        digest.update(relative)
        digest.update(len(contents).to_bytes(8, "little"))
        digest.update(contents)
        total_bytes += len(contents)
    return {
        "sha256": digest.hexdigest(),
        "file_count": len(selected),
        "bytes": total_bytes,
    }


def run_case(
    engine: Path,
    engine_version: str,
    configuration: str,
    duration_ms: int,
    distribution_root: Path,
) -> dict[str, object]:
    case_name = f"godot-{engine_version}-{configuration}"
    case_root = CLEAN_ROOT / case_name
    package = copy_clean_project(
        case_root, configuration, distribution_root
    )
    import_result = subprocess.run(
        [
            str(engine),
            "--headless",
            "--path",
            str(case_root),
            "--import",
        ],
        cwd=case_root,
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
        timeout=120,
    )
    ARTIFACT_ROOT.mkdir(parents=True, exist_ok=True)
    (ARTIFACT_ROOT / f"{case_name}.import.stdout.txt").write_text(
        import_result.stdout, encoding="utf-8"
    )
    (ARTIFACT_ROOT / f"{case_name}.import.stderr.txt").write_text(
        import_result.stderr, encoding="utf-8"
    )
    import_combined = import_result.stdout + import_result.stderr
    print(
        import_combined,
        end="" if import_combined.endswith("\n") else "\n",
    )
    extension_cache = case_root / ".godot" / "extension_list.cfg"
    if (
        "ERROR:" in import_combined
        or not extension_cache.is_file()
        or (
            "res://addons/world_transvoxel/world_transvoxel.gdextension"
            not in extension_cache.read_text(encoding="utf-8")
        )
    ):
        raise RuntimeError(f"PQ3 clean import failed for {case_name}.")
    command = [
        str(engine),
        "--headless",
        "--max-fps",
        "120",
        "--path",
        str(case_root),
        "--script",
        "res://tests/godot/production_full_world_soak.gd",
        "--",
        "--duration-ms",
        str(duration_ms),
    ]
    result = subprocess.run(
        command,
        cwd=case_root,
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
        timeout=max(180, duration_ms // 1000 + 150),
    )
    (ARTIFACT_ROOT / f"{case_name}.stdout.txt").write_text(
        result.stdout, encoding="utf-8"
    )
    (ARTIFACT_ROOT / f"{case_name}.stderr.txt").write_text(
        result.stderr, encoding="utf-8"
    )
    combined = result.stdout + result.stderr
    print(combined, end="" if combined.endswith("\n") else "\n")
    matches = re.findall(
        rf"^{re.escape(PASS_MARKER)}(\{{.*\}})$",
        combined,
        flags=re.MULTILINE,
    )
    if result.returncode != 0 or len(matches) != 1:
        raise RuntimeError(f"PQ3 clean-install soak failed for {case_name}.")
    runtime = json.loads(matches[0])
    if (
        runtime.get("actual_duration_ms", 0) < duration_ms
        or runtime.get("world_pages") != 28
        or runtime.get("queried_pages") != 28
        or runtime.get("edits", 0) <= 0
        or runtime.get("transition_checks", 0) <= 0
        or runtime.get("shutdown") != "clean"
    ):
        raise RuntimeError(f"PQ3 evidence is incomplete for {case_name}.")
    return {
        "engine_version": engine_version,
        "configuration": configuration,
        "engine_sha256": sha256(engine),
        "runtime_binary_sha256": sha256(
            distribution_binary_path(distribution_root, configuration)
        ),
        "distribution": package,
        "runtime": runtime,
    }


def test_pq3(
    *,
    skip_build: bool = False,
    skip_engine_download: bool = False,
    duration_ms: int = DEFAULT_DURATION_MS,
    write_reference_evidence: bool = False,
    prepare_fixture: bool = True,
    distribution_root: Path = REPO_ROOT,
) -> dict[str, object]:
    require_supported_python()
    distribution_root = distribution_root.resolve()
    if not (
        distribution_root
        / "addons"
        / "world_transvoxel"
        / "world_transvoxel.gdextension"
    ).is_file():
        raise RuntimeError(
            f"PQ3 distribution root is incomplete: {distribution_root}"
        )
    if duration_ms < 1000 or duration_ms > 300000:
        raise RuntimeError("PQ3 duration must be between 1,000 and 300,000 ms.")
    if not skip_build:
        build("all")
    if not skip_engine_download:
        download_test_engines()
    if prepare_fixture:
        prepare_example_world("template_release")
    elif not (
        REPO_ROOT / "build" / "world-transvoxel-example" / "world.wtworld"
    ).is_file():
        raise RuntimeError("PQ3 baked-world fixture is missing.")
    remove_tree(CLEAN_ROOT, REPO_ROOT / "build")
    CLEAN_ROOT.mkdir(parents=True)
    cases: list[dict[str, object]] = []
    for configuration in ("template_debug", "template_release"):
        for spec in engine_specs():
            engine = engine_executable(spec)
            if not engine.is_file():
                raise RuntimeError(f"Required PQ3 engine is missing: {engine}")
            cases.append(
                run_case(
                    engine,
                    spec.version,
                    configuration,
                    duration_ms,
                    distribution_root,
                )
            )
    host = host_platform()
    evidence: dict[str, object] = {
        "schema": 1,
        "scope": "pq3_clean_install_full_world_soak",
        "host": {
            "system": platform.system(),
            "architecture": platform.machine(),
            "godot_platform": host.godot_platform,
            "godot_arch": host.godot_arch,
            "python": platform.python_version(),
        },
        "requested_duration_ms_per_case": duration_ms,
        "distribution_manifest_sha256": (
            sha256(distribution_root / "RELEASE_MANIFEST.json")
            if (distribution_root / "RELEASE_MANIFEST.json").is_file()
            else None
        ),
        "case_count": len(cases),
        "cases": cases,
        "acceptance": {
            "clean_project_boundary": True,
            "required_notices": True,
            "official_mit_backend": True,
            "all_baked_pages_queried": True,
            "real_render_collision_transitions": True,
            "durable_edit_query_ordering": True,
            "compaction_reopen": True,
            "migration_reopen": True,
            "bounded_memory_queues_frames": True,
            "clean_shutdown": True,
        },
    }
    ARTIFACT_ROOT.mkdir(parents=True, exist_ok=True)
    artifact_path = ARTIFACT_ROOT / (
        f"pq3_clean_install_soak_{host.godot_platform}_{host.godot_arch}.json"
    )
    artifact_path.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    if write_reference_evidence:
        REFERENCE_EVIDENCE.write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    remove_tree(CLEAN_ROOT, REPO_ROOT / "build")
    print(
        "PQ3_CLEAN_INSTALL_SOAK_PASS "
        f"cases={len(cases)} duration_ms={duration_ms} "
        f"evidence={artifact_path}"
    )
    return evidence


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run the isolated PQ3 clean-install full-world soak matrix."
    )
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    parser.add_argument(
        "--duration-ms", type=int, default=DEFAULT_DURATION_MS
    )
    parser.add_argument("--write-reference-evidence", action="store_true")
    parser.add_argument(
        "--distribution-root", type=Path, default=REPO_ROOT
    )
    arguments = parser.parse_args()
    test_pq3(
        skip_build=arguments.skip_build,
        skip_engine_download=arguments.skip_engine_download,
        duration_ms=arguments.duration_ms,
        write_reference_evidence=arguments.write_reference_evidence,
        prepare_fixture=True,
        distribution_root=arguments.distribution_root,
    )


if __name__ == "__main__":
    main()
