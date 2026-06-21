#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
TOOLS_ROOT = REPO_ROOT / ".tools"
DOWNLOAD_ROOT = TOOLS_ROOT / "downloads"
USER_AGENT = "world-transvoxel-tooling/1"


@dataclass(frozen=True)
class HostPlatform:
    godot_platform: str
    godot_arch: str
    zig_target: str
    executable_suffix: str
    library_prefix: str
    library_suffix: str


def require_supported_python() -> None:
    if sys.version_info < (3, 11):
        raise RuntimeError("World Transvoxel tooling requires Python 3.11 or newer.")


def host_platform() -> HostPlatform:
    machine = platform.machine().lower()
    if machine in {"amd64", "x86_64"}:
        zig_arch = "x86_64"
        godot_arch = "x86_64"
    elif machine in {"arm64", "aarch64"}:
        zig_arch = "aarch64"
        godot_arch = "arm64"
    else:
        raise RuntimeError(f"Unsupported host architecture: {platform.machine()}")

    if sys.platform == "win32":
        return HostPlatform(
            "windows",
            godot_arch,
            f"{zig_arch}-windows",
            ".exe",
            "",
            ".dll",
        )
    if sys.platform.startswith("linux"):
        return HostPlatform(
            "linux",
            godot_arch,
            f"{zig_arch}-linux",
            "",
            "lib",
            ".so",
        )
    if sys.platform == "darwin":
        return HostPlatform(
            "macos",
            godot_arch,
            f"{zig_arch}-macos",
            "",
            "lib",
            ".dylib",
        )
    raise RuntimeError(f"Unsupported host operating system: {sys.platform}")


def ensure_within(path: Path, root: Path) -> Path:
    resolved_path = path.resolve()
    resolved_root = root.resolve()
    try:
        resolved_path.relative_to(resolved_root)
    except ValueError as error:
        raise RuntimeError(f"Path escaped {resolved_root}: {resolved_path}") from error
    return resolved_path


def remove_tree(path: Path, root: Path) -> None:
    target = ensure_within(path, root)
    if target == root.resolve():
        raise RuntimeError(f"Refusing to remove root directory: {target}")
    if target.exists():
        shutil.rmtree(target)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def download_file(url: str, target: Path, refresh: bool = False) -> None:
    if target.is_file() and not refresh:
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(f"{target.name}.part")
    if temporary.exists():
        temporary.unlink()
    print(f"Downloading {url}")
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output, length=1024 * 1024)
        temporary.replace(target)
    finally:
        if temporary.exists():
            temporary.unlink()


def _safe_archive_target(root: Path, member_name: str) -> None:
    target = (root / member_name).resolve()
    ensure_within(target, root)


def extract_archive(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as bundle:
            for member in bundle.infolist():
                _safe_archive_target(destination, member.filename)
            bundle.extractall(destination)
        return
    if archive.name.endswith(".tar.xz"):
        with tarfile.open(archive, "r:xz") as bundle:
            for member in bundle.getmembers():
                _safe_archive_target(destination, member.name)
                if member.issym() or member.islnk():
                    raise RuntimeError(f"Archive contains a link: {member.name}")
            bundle.extractall(destination)
        return
    raise RuntimeError(f"Unsupported archive format: {archive}")


def run(
    command: Sequence[os.PathLike[str] | str],
    *,
    cwd: Path = REPO_ROOT,
    capture: bool = False,
    env: Mapping[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    normalized = [os.fspath(argument) for argument in command]
    return subprocess.run(
        normalized,
        cwd=cwd,
        check=True,
        text=True,
        capture_output=capture,
        env=None if env is None else dict(env),
    )


def output(command: Sequence[os.PathLike[str] | str], *, cwd: Path = REPO_ROOT) -> str:
    return run(command, cwd=cwd, capture=True).stdout.strip()


def native_test_path(
    configuration: str,
    test_name: str = "test_wt_m1_cell_backend",
) -> Path:
    host = host_platform()
    return (
        REPO_ROOT
        / "build"
        / "native-tests"
        / (
            f"{test_name}.{configuration}."
            f"{host.godot_arch}{host.executable_suffix}"
        )
    )


def native_tool_path(
    configuration: str,
    tool_name: str,
) -> Path:
    host = host_platform()
    return (
        REPO_ROOT
        / "build"
        / "tools"
        / (
            f"{tool_name}.{configuration}."
            f"{host.godot_arch}{host.executable_suffix}"
        )
    )


def addon_binary_path(configuration: str) -> Path:
    host = host_platform()
    filename = (
        f"{host.library_prefix}world_transvoxel.{host.godot_platform}."
        f"{configuration}.{host.godot_arch}{host.library_suffix}"
    )
    return REPO_ROOT / "addons" / "world_transvoxel" / "bin" / filename
