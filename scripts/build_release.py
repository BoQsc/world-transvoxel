#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

from wt_script_common import (
    REPO_ROOT,
    addon_binary_path,
    host_platform,
    native_tool_path,
    remove_tree,
    require_supported_python,
    sha256,
)


RELEASE_VERSION = "1.0.3"
RELEASE_NAME = f"world-transvoxel-{RELEASE_VERSION}-windows-x86_64"
DEFAULT_RELEASE_ROOT = REPO_ROOT / "artifacts" / "release" / RELEASE_NAME
NOTICE_PATHS = (
    Path("LICENSE"),
    Path("LICENSE_SCOPE.md"),
    Path("LICENSES/MIT-Transvoxel.txt"),
)
TOOL_NAMES = ("wt_bake_tool", "wt_storage_tool")
SUPPORTED_GDEXTENSION = """[configuration]

entry_symbol = "world_transvoxel_library_init"
compatibility_minimum = "4.6"
reloadable = true

[libraries]

windows.debug.x86_64 = "res://addons/world_transvoxel/bin/world_transvoxel.windows.template_debug.x86_64.dll"
windows.release.x86_64 = "res://addons/world_transvoxel/bin/world_transvoxel.windows.template_release.x86_64.dll"
"""


def guard_repository_scan(destination: Path) -> None:
    resolved = destination.resolve()
    for name in ("build", "artifacts"):
        generated_root = (REPO_ROOT / name).resolve()
        try:
            resolved.relative_to(generated_root)
        except ValueError:
            continue
        generated_root.mkdir(parents=True, exist_ok=True)
        (generated_root / ".gdignore").write_text("", encoding="utf-8")
        return


def read_version() -> str:
    header = (
        REPO_ROOT
        / "addons"
        / "world_transvoxel"
        / "src"
        / "core"
        / "wt_version.h"
    ).read_text(encoding="utf-8")
    match = re.search(r'kAddonVersion\s*=\s*"([^"]+)"', header)
    if match is None:
        raise RuntimeError("Could not read release version.")
    return match.group(1)


def file_records(root: Path) -> tuple[list[dict[str, object]], str]:
    paths = sorted(
        (
            path
            for path in root.rglob("*")
            if path.is_file() and path.name != "RELEASE_MANIFEST.json"
        ),
        key=lambda path: path.relative_to(root).as_posix(),
    )
    digest = hashlib.sha256()
    records: list[dict[str, object]] = []
    for path in paths:
        relative = path.relative_to(root).as_posix()
        contents = path.read_bytes()
        encoded = relative.encode("utf-8")
        digest.update(len(encoded).to_bytes(4, "little"))
        digest.update(encoded)
        digest.update(len(contents).to_bytes(8, "little"))
        digest.update(contents)
        records.append(
            {
                "path": relative,
                "bytes": len(contents),
                "sha256": hashlib.sha256(contents).hexdigest(),
            }
        )
    return records, digest.hexdigest()


def materialize_release(destination: Path) -> dict[str, object]:
    require_supported_python()
    host = host_platform()
    if (host.godot_platform, host.godot_arch) != ("windows", "x86_64"):
        raise RuntimeError("World Transvoxel 1.0.x release is Windows x86-64 only.")
    if read_version() != RELEASE_VERSION:
        raise RuntimeError("Release script and addon versions disagree.")
    destination = destination.resolve()
    guard_repository_scan(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    remove_tree(destination, destination.parent)
    destination.mkdir()

    for relative in NOTICE_PATHS:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO_ROOT / relative, target)

    source_addon = REPO_ROOT / "addons" / "world_transvoxel"
    target_addon = destination / "addons" / "world_transvoxel"
    shutil.copytree(
        source_addon,
        target_addon,
        ignore=shutil.ignore_patterns(
            "bin",
            "__pycache__",
            "*.pyc",
            "*.o",
            "*.obj",
            "*.a",
            "*.lib",
            "*.pdb",
            "*.exp",
            "*.idb",
            "*.ilk",
        ),
    )
    (target_addon / "world_transvoxel.gdextension").write_text(
        SUPPORTED_GDEXTENSION, encoding="utf-8"
    )
    target_bin = target_addon / "bin"
    target_bin.mkdir()
    for configuration in ("template_debug", "template_release"):
        runtime = addon_binary_path(configuration)
        if not runtime.is_file():
            raise RuntimeError(f"Missing release runtime binary: {runtime}")
        shutil.copy2(runtime, target_bin / runtime.name)
        for tool_name in TOOL_NAMES:
            tool = native_tool_path(configuration, tool_name)
            if not tool.is_file():
                raise RuntimeError(f"Missing release native tool: {tool}")
            packaged_name = (
                f"{tool_name}.windows.{configuration}.x86_64.exe"
            )
            shutil.copy2(tool, target_bin / packaged_name)

    records, content_sha256 = file_records(destination)
    manifest: dict[str, object] = {
        "schema": "world-transvoxel.release.v1",
        "product": "World Transvoxel",
        "version": RELEASE_VERSION,
        "milestone": "PQ4",
        "artifact": {
            "format": "directory",
            "name": RELEASE_NAME,
            "content_sha256": content_sha256,
            "file_count": len(records),
            "bytes": sum(int(record["bytes"]) for record in records),
        },
        "support": {
            "operating_system": "Windows 10/11",
            "architecture": "x86_64",
            "godot_versions": ["4.6.3", "4.7"],
            "runtime_configurations": [
                "template_debug",
                "template_release",
            ],
            "python_tools_minimum": "3.11",
        },
        "backend": {
            "id": "transvoxel_mit_official",
            "license": "MIT",
            "upstream_revision": (
                "51a494f03c5b024cd153b596bcc7152eb3cc93a6"
            ),
            "transvoxel_cpp_sha256": (
                "83a5511346b54c42e4e66dec916d3971c92f4fbda1c7878cbad5901a820dcab4"
            ),
        },
        "toolchain": {
            "zig": "0.16.0",
            "godot_cpp_revision": (
                "e83fd0904c13356ed1d4c3d09f8bb9132bdc6b77"
            ),
        },
        "files": records,
    }
    (destination / "RELEASE_MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build the deterministic World Transvoxel release directory."
    )
    parser.add_argument("--output", type=Path, default=DEFAULT_RELEASE_ROOT)
    arguments = parser.parse_args()
    manifest = materialize_release(arguments.output)
    print(
        "WORLD_TRANSVOXEL_RELEASE_DIRECTORY_PASS "
        f"path={arguments.output.resolve()} "
        f"sha256={manifest['artifact']['content_sha256']}"
    )


if __name__ == "__main__":
    main()
