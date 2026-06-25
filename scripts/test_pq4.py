#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from build import build
from build_release import (
    DEFAULT_RELEASE_ROOT,
    RELEASE_NAME,
    RELEASE_VERSION,
    materialize_release,
)
from download_test_engines import download_test_engines
from test_pq3 import DEFAULT_DURATION_MS, test_pq3
from wt_script_common import (
    REPO_ROOT,
    addon_binary_path,
    host_platform,
    native_tool_path,
    remove_tree,
    require_supported_python,
    sha256,
)


REPRO_ROOT = REPO_ROOT / "build" / "pq4-repro"
REFERENCE_EVIDENCE = (
    REPO_ROOT
    / "docs"
    / "evidence"
    / "pq4_release_windows_x86_64.json"
)
TRANSVOXEL_CPP_SHA256 = (
    "83a5511346b54c42e4e66dec916d3971c92f4fbda1c7878cbad5901a820dcab4"
)
FORBIDDEN_RELEASE_SUFFIXES = {
    ".a",
    ".exp",
    ".idb",
    ".ilk",
    ".lib",
    ".o",
    ".obj",
    ".pdb",
}


def clean_release_outputs() -> None:
    host = host_platform()
    if (host.godot_platform, host.godot_arch) != ("windows", "x86_64"):
        raise RuntimeError("PQ4 is qualified only for Windows x86-64.")
    build_root = REPO_ROOT / "build"
    for configuration in ("template_debug", "template_release"):
        remove_tree(
            build_root
            / "world_transvoxel"
            / f"windows.{configuration}.x86_64",
            build_root,
        )
        runtime = addon_binary_path(configuration)
        if runtime.exists():
            runtime.unlink()
        for tool_name in ("wt_bake_tool", "wt_storage_tool"):
            tool = native_tool_path(configuration, tool_name)
            if tool.exists():
                tool.unlink()


def directory_bytes(root: Path) -> dict[str, bytes]:
    return {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def directory_identity(root: Path) -> dict[str, object]:
    files = directory_bytes(root)
    digest = hashlib.sha256()
    for relative, contents in files.items():
        encoded = relative.encode("utf-8")
        digest.update(len(encoded).to_bytes(4, "little"))
        digest.update(encoded)
        digest.update(len(contents).to_bytes(8, "little"))
        digest.update(contents)
    return {
        "sha256": digest.hexdigest(),
        "file_count": len(files),
        "bytes": sum(len(contents) for contents in files.values()),
    }


def verify_reproducible(first: Path, second: Path) -> dict[str, object]:
    first_files = directory_bytes(first)
    second_files = directory_bytes(second)
    if first_files.keys() != second_files.keys():
        missing = sorted(first_files.keys() - second_files.keys())
        extra = sorted(second_files.keys() - first_files.keys())
        raise RuntimeError(
            f"PQ4 release paths differ; missing={missing}, extra={extra}"
        )
    mismatched = [
        relative
        for relative in first_files
        if first_files[relative] != second_files[relative]
    ]
    if mismatched:
        raise RuntimeError(
            f"PQ4 release bytes differ: {mismatched[:20]}"
        )
    first_identity = directory_identity(first)
    second_identity = directory_identity(second)
    if first_identity != second_identity:
        raise RuntimeError("PQ4 release identities differ.")
    return first_identity


def audit_release(root: Path) -> dict[str, object]:
    expected_top_level = {
        "LICENSE",
        "LICENSE_SCOPE.md",
        "LICENSES",
        "RELEASE_MANIFEST.json",
        "addons",
    }
    if {entry.name for entry in root.iterdir()} != expected_top_level:
        raise RuntimeError("PQ4 release top-level boundary is invalid.")
    for path in root.rglob("*"):
        if path.is_symlink():
            raise RuntimeError(f"PQ4 release contains a link: {path}")
        if path.is_file() and path.suffix.lower() in FORBIDDEN_RELEASE_SUFFIXES:
            relative = path.relative_to(root).as_posix()
            raise RuntimeError(
                f"PQ4 release contains a native build intermediate: {relative}"
            )

    addon = root / "addons" / "world_transvoxel"
    required = (
        addon / "PUBLIC_API.md",
        addon / "OPERATING_LIMITS.md",
        addon / "tools" / "wt_bake.py",
        addon / "tools" / "wt_storage.py",
        addon / "thirdparty" / "transvoxel_mit" / "LICENSE",
        addon / "thirdparty" / "transvoxel_mit" / "UPSTREAM.md",
    )
    if not all(path.is_file() for path in required):
        raise RuntimeError("PQ4 release documentation/tool boundary is incomplete.")

    descriptor = (addon / "world_transvoxel.gdextension").read_text(
        encoding="utf-8"
    )
    expected_mappings = {
        "windows.debug.x86_64",
        "windows.release.x86_64",
    }
    mappings = {
        line.split("=", 1)[0].strip()
        for line in descriptor.splitlines()
        if line.strip().startswith(("windows.", "linux.", "macos."))
    }
    if mappings != expected_mappings:
        raise RuntimeError(f"PQ4 release support mappings are invalid: {mappings}")

    notice_path = root / "LICENSES" / "MIT-Transvoxel.txt"
    notice = notice_path.read_bytes()
    if (
        (root / "LICENSE").read_bytes() != (REPO_ROOT / "LICENSE").read_bytes()
        or (root / "LICENSE_SCOPE.md").read_bytes()
        != (REPO_ROOT / "LICENSE_SCOPE.md").read_bytes()
        or notice != (REPO_ROOT / "LICENSES" / "MIT-Transvoxel.txt").read_bytes()
        or (
            addon / "thirdparty" / "transvoxel_mit" / "LICENSE"
        ).read_text(encoding="utf-8").splitlines()
        != notice_path.read_text(encoding="utf-8").splitlines()
    ):
        raise RuntimeError("PQ4 release license notices are incomplete.")
    official = addon / "thirdparty" / "transvoxel_mit" / "Transvoxel.cpp"
    if sha256(official) != TRANSVOXEL_CPP_SHA256:
        raise RuntimeError("PQ4 official Transvoxel.cpp provenance mismatch.")

    for script_name in ("wt_bake.py", "wt_storage.py"):
        script = (addon / "tools" / script_name).read_text(encoding="utf-8")
        if "wt_script_common" in script or "bootstrap_toolchain" in script:
            raise RuntimeError(f"PQ4 packaged {script_name} imports repository code.")

    streaming_source = (
        addon
        / "src"
        / "api"
        / "world_transvoxel_terrain_streaming.cpp"
    ).read_text(encoding="utf-8")
    metrics_source = (
        addon
        / "src"
        / "api"
        / "world_transvoxel_terrain_metrics.cpp"
    ).read_text(encoding="utf-8")
    render_sink_source = (
        addon / "src" / "render" / "wt_godot_render_sink.cpp"
    ).read_text(encoding="utf-8")
    operating_limits = (addon / "OPERATING_LIMITS.md").read_text(
        encoding="utf-8"
    )
    if (
        "kChunkRetirementFlushBudget = 4U" not in streaming_source
        or "ready chunk retirement removals per frame | 4" not in operating_limits
    ):
        raise RuntimeError("PQ4 release retirement flush budget is not locked.")
    if (
        "kRenderRetirementFadeFrames = 12U" not in render_sink_source
        or "kRenderIntroductionFadeFrames = 12U" not in render_sink_source
        or "render_fading_resources" not in metrics_source
        or "render retirement fade duration | 12 frames" not in operating_limits
        or "render introduction fade duration | 12 frames" not in operating_limits
    ):
        raise RuntimeError("PQ4 release render fade windows are not locked.")

    manifest_path = root / "RELEASE_MANIFEST.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (
        manifest.get("schema") != "world-transvoxel.release.v1"
        or manifest.get("version") != RELEASE_VERSION
        or manifest.get("artifact", {}).get("name") != RELEASE_NAME
    ):
        raise RuntimeError("PQ4 release manifest identity is invalid.")
    records = manifest.get("files", [])
    record_paths = [record.get("path") for record in records]
    if not all(isinstance(path, str) for path in record_paths):
        raise RuntimeError("PQ4 release manifest contains an invalid path.")
    if len(record_paths) != len(set(record_paths)):
        raise RuntimeError("PQ4 release manifest contains duplicate paths.")
    payload_paths = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file() and path != manifest_path
    }
    listed_paths = set(record_paths)
    if payload_paths != listed_paths:
        missing = sorted(listed_paths - payload_paths)
        unlisted = sorted(payload_paths - listed_paths)
        raise RuntimeError(
            "PQ4 release payload and manifest differ; "
            f"missing={missing[:20]}, unlisted={unlisted[:20]}"
        )
    for record in records:
        path = root / record["path"]
        if (
            not path.is_file()
            or path.stat().st_size != record["bytes"]
            or sha256(path) != record["sha256"]
        ):
            raise RuntimeError(
                f"PQ4 release manifest file mismatch: {record['path']}"
            )
    if (
        manifest["artifact"].get("file_count") != len(records)
        or manifest["artifact"].get("bytes")
        != sum(int(record["bytes"]) for record in records)
    ):
        raise RuntimeError("PQ4 release manifest aggregate metadata is invalid.")
    return {
        "manifest_sha256": sha256(manifest_path),
        "content_sha256": manifest["artifact"]["content_sha256"],
        "file_count": manifest["artifact"]["file_count"],
        "bytes": manifest["artifact"]["bytes"],
        "official_transvoxel_cpp_sha256": sha256(official),
    }


def test_packaged_tools(root: Path) -> dict[str, object]:
    addon = root / "addons" / "world_transvoxel"
    with tempfile.TemporaryDirectory(
        prefix="world-transvoxel-pq4-tool-"
    ) as directory:
        temporary = Path(directory)
        density = temporary / "density.f32"
        keys = temporary / "keys.txt"
        output = temporary / "world"
        values = bytearray()
        for z in range(-1, 18):
            for y in range(-1, 18):
                for x in range(-1, 18):
                    values.extend(struct.pack("<f", float(x + y + z)))
        density.write_bytes(values)
        keys.write_text("0 0 0 0\n", encoding="utf-8")
        result = subprocess.run(
            [
                sys.executable,
                str(addon / "tools" / "wt_bake.py"),
                str(density),
                str(keys),
                str(output),
                "--origin",
                "-1",
                "-1",
                "-1",
                "--dimensions",
                "19",
                "19",
                "19",
                "--source-revision",
                "9001",
                "--configuration",
                "template_release",
            ],
            cwd=temporary,
            check=False,
            text=True,
            capture_output=True,
            errors="replace",
        )
        if result.returncode != 0:
            raise RuntimeError(result.stdout + result.stderr)
        bake = json.loads(result.stdout)
        validation = subprocess.run(
            [
                sys.executable,
                str(addon / "tools" / "wt_storage.py"),
                "validate",
                str(output / "world.wtworld"),
            ],
            cwd=temporary,
            check=False,
            text=True,
            capture_output=True,
            errors="replace",
        )
        if validation.returncode != 0:
            raise RuntimeError(validation.stdout + validation.stderr)
        world = json.loads(validation.stdout)
        if (
            bake.get("pages") != 1
            or bake.get("bounded") is not True
            or bake.get("peak_page_payloads") != 1
            or bake.get("source_cache_bytes") != 196_608
            or world.get("pages") != 1
            or world.get("source_revision") != 9001
        ):
            raise RuntimeError("PQ4 packaged tool metadata is invalid.")
        return {
            "pages": 1,
            "world_sha256": world["sha256"],
            "bake_report_sha256": bake["world_sha256"],
            "bounded": True,
            "peak_page_payloads": 1,
            "source_cache_bytes": 196_608,
        }


def summarize_pq3(evidence: dict[str, object]) -> dict[str, object]:
    return {
        "case_count": evidence["case_count"],
        "requested_duration_ms_per_case": (
            evidence["requested_duration_ms_per_case"]
        ),
        "distribution_manifest_sha256": (
            evidence["distribution_manifest_sha256"]
        ),
        "cases": [
            {
                "engine_version": case["engine_version"],
                "configuration": case["configuration"],
                "runtime_binary_sha256": case["runtime_binary_sha256"],
                "distribution_sha256": case["distribution"]["sha256"],
                "actual_duration_ms": case["runtime"]["actual_duration_ms"],
                "shutdown": case["runtime"]["shutdown"],
            }
            for case in evidence["cases"]
        ],
    }


def test_pq4(
    *,
    skip_build: bool = False,
    skip_engine_download: bool = False,
    duration_ms: int = DEFAULT_DURATION_MS,
    write_reference_evidence: bool = False,
) -> dict[str, object]:
    require_supported_python()
    if not skip_engine_download:
        download_test_engines()
    remove_tree(REPRO_ROOT, REPO_ROOT / "build")
    first = REPRO_ROOT / "first" / RELEASE_NAME
    second = REPRO_ROOT / "second" / RELEASE_NAME

    if not skip_build:
        clean_release_outputs()
        build("all", skip_bootstrap=True)
    materialize_release(first)
    if not skip_build:
        clean_release_outputs()
        build("all", skip_bootstrap=True)
    materialize_release(second)

    identity = verify_reproducible(first, second)
    audit = audit_release(second)
    tools = test_packaged_tools(second)
    pq3 = test_pq3(
        skip_build=True,
        skip_engine_download=True,
        duration_ms=duration_ms,
        write_reference_evidence=True,
        prepare_fixture=True,
        distribution_root=second,
    )

    remove_tree(DEFAULT_RELEASE_ROOT, DEFAULT_RELEASE_ROOT.parent)
    DEFAULT_RELEASE_ROOT.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(second, DEFAULT_RELEASE_ROOT)
    if directory_identity(DEFAULT_RELEASE_ROOT) != identity:
        raise RuntimeError("Published PQ4 release differs from qualified release.")

    host = host_platform()
    evidence: dict[str, object] = {
        "schema": 1,
        "scope": "pq4_production_release",
        "version": RELEASE_VERSION,
        "artifact": {
            "path": f"artifacts/release/{RELEASE_NAME}",
            **identity,
            **audit,
            "format": "directory",
            "reproducible_builds": 2,
        },
        "host": {
            "system": platform.system(),
            "architecture": platform.machine(),
            "godot_platform": host.godot_platform,
            "godot_arch": host.godot_arch,
            "python": platform.python_version(),
        },
        "packaged_tools": tools,
        "clean_install_soak": summarize_pq3(pq3),
        "acceptance": {
            "byte_identical_clean_builds": True,
            "directory_release_no_archive": True,
            "qualified_support_matrix_only": True,
            "public_api_documented": True,
            "operational_limits_documented": True,
            "ready_chunk_retirement_bounded": True,
            "ready_chunk_retirement_fade_bounded": True,
            "ready_chunk_introduction_fade_bounded": True,
            "license_notices_complete": True,
            "official_mit_provenance_verified": True,
            "self_contained_bake_and_storage_tools": True,
            "exact_release_install_matrix_passed": True,
        },
    }
    artifact_evidence = (
        REPO_ROOT
        / "artifacts"
        / "release"
        / "pq4_release_windows_x86_64.json"
    )
    artifact_evidence.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    if write_reference_evidence:
        REFERENCE_EVIDENCE.write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    remove_tree(REPRO_ROOT, REPO_ROOT / "build")
    print(
        "PQ4_PRODUCTION_RELEASE_PASS "
        f"version={RELEASE_VERSION} sha256={identity['sha256']} "
        f"path={DEFAULT_RELEASE_ROOT}"
    )
    return evidence


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run the finite PQ4 production release qualification."
    )
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    parser.add_argument("--duration-ms", type=int, default=DEFAULT_DURATION_MS)
    parser.add_argument("--write-reference-evidence", action="store_true")
    arguments = parser.parse_args()
    test_pq4(
        skip_build=arguments.skip_build,
        skip_engine_download=arguments.skip_engine_download,
        duration_ms=arguments.duration_ms,
        write_reference_evidence=arguments.write_reference_evidence,
    )


if __name__ == "__main__":
    main()
