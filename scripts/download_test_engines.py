#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import stat
from dataclasses import dataclass
from pathlib import Path

from wt_script_common import (
    DOWNLOAD_ROOT,
    TOOLS_ROOT,
    HostPlatform,
    download_file,
    extract_archive,
    host_platform,
    output,
    remove_tree,
    sha256,
)


@dataclass(frozen=True)
class EngineSpec:
    version: str
    tag: str
    archive_name: str
    executable_relative: Path
    sha256: str

    @property
    def url(self) -> str:
        return (
            "https://github.com/godotengine/godot/releases/download/"
            f"{self.tag}/{self.archive_name}"
        )


ENGINE_DATA = {
    "4.6.3": {
        "tag": "4.6.3-stable",
        "windows-x86_64": (
            "Godot_v4.6.3-stable_win64.exe.zip",
            "Godot_v4.6.3-stable_win64.exe",
            "e39986a178d585ce7ac198fb8de6ea436366dc0cc00e594810c2e3e104c04b90",
        ),
        "windows-arm64": (
            "Godot_v4.6.3-stable_windows_arm64.exe.zip",
            "Godot_v4.6.3-stable_windows_arm64.exe",
            "d53b5b4c1d1e4f242d490a98a0da1b3579628e4d6f98a052542599862a96b10f",
        ),
        "linux-x86_64": (
            "Godot_v4.6.3-stable_linux.x86_64.zip",
            "Godot_v4.6.3-stable_linux.x86_64",
            "d0bc2113065e481c9c2c2b2c37daa4e8be3fe9e27f0ab9ab0b6096e9a37907f3",
        ),
        "linux-arm64": (
            "Godot_v4.6.3-stable_linux.arm64.zip",
            "Godot_v4.6.3-stable_linux.arm64",
            "90c70382eee1542904bf507b9bdc6e62a230ac73fd214bf3887a9e0a4d85aeed",
        ),
        "macos-x86_64": (
            "Godot_v4.6.3-stable_macos.universal.zip",
            "Godot.app/Contents/MacOS/Godot",
            "30630f3e9b11e10b35c1f90ba8814185dcec43fae1a48345159be7552c64bfe8",
        ),
        "macos-arm64": (
            "Godot_v4.6.3-stable_macos.universal.zip",
            "Godot.app/Contents/MacOS/Godot",
            "30630f3e9b11e10b35c1f90ba8814185dcec43fae1a48345159be7552c64bfe8",
        ),
    },
    "4.7": {
        "tag": "4.7-stable",
        "windows-x86_64": (
            "Godot_v4.7-stable_win64.exe.zip",
            "Godot_v4.7-stable_win64.exe",
            "02a5312236f4e0209c78bcb2f52135b1963e6b8888c873c9cee81459e60bcd71",
        ),
        "windows-arm64": (
            "Godot_v4.7-stable_windows_arm64.exe.zip",
            "Godot_v4.7-stable_windows_arm64.exe",
            "45ae7d4d782211d6060e6ef7adca0df21a7509e8c0b545db11fcaf7f0582db1f",
        ),
        "linux-x86_64": (
            "Godot_v4.7-stable_linux.x86_64.zip",
            "Godot_v4.7-stable_linux.x86_64",
            "0b1a6c54c2c619c12e169fe9241edda4b81080b519451cec2984bf0d2c6cb73c",
        ),
        "linux-arm64": (
            "Godot_v4.7-stable_linux.arm64.zip",
            "Godot_v4.7-stable_linux.arm64",
            "db5aa126353a18fd664818e4f1b9cfffaa77e32d4c9af0ea87e8f028a395a1ed",
        ),
        "macos-x86_64": (
            "Godot_v4.7-stable_macos.universal.zip",
            "Godot.app/Contents/MacOS/Godot",
            "a6708c336f690e0dd8abd3d587d661707f4f33ed436946a3ec000d2fb497fd6c",
        ),
        "macos-arm64": (
            "Godot_v4.7-stable_macos.universal.zip",
            "Godot.app/Contents/MacOS/Godot",
            "a6708c336f690e0dd8abd3d587d661707f4f33ed436946a3ec000d2fb497fd6c",
        ),
    },
}


def engine_specs(host: HostPlatform | None = None) -> tuple[EngineSpec, ...]:
    selected_host = host or host_platform()
    key = f"{selected_host.godot_platform}-{selected_host.godot_arch}"
    specs = []
    for version, data in ENGINE_DATA.items():
        archive_name, executable, expected_hash = data[key]
        specs.append(
            EngineSpec(
                version,
                data["tag"],
                archive_name,
                Path(executable),
                expected_hash,
            )
        )
    return tuple(specs)


def engine_executable(spec: EngineSpec) -> Path:
    return TOOLS_ROOT / "godot" / spec.version / spec.executable_relative


def download_test_engines(refresh: bool = False) -> None:
    engine_root = TOOLS_ROOT / "godot"
    DOWNLOAD_ROOT.mkdir(parents=True, exist_ok=True)
    engine_root.mkdir(parents=True, exist_ok=True)

    for spec in engine_specs():
        archive_path = DOWNLOAD_ROOT / spec.archive_name
        version_root = engine_root / spec.version
        executable = engine_executable(spec)
        download_file(spec.url, archive_path, refresh)
        actual_hash = sha256(archive_path)
        if actual_hash != spec.sha256:
            raise RuntimeError(
                f"Godot {spec.version} archive hash mismatch: "
                f"expected {spec.sha256}, got {actual_hash}"
            )

        if refresh or not executable.is_file():
            remove_tree(version_root, engine_root)
            extract_archive(archive_path, version_root)
        if os.name != "nt":
            executable.chmod(executable.stat().st_mode | stat.S_IXUSR)

        actual_version = output([executable, "--headless", "--version"])
        print(f"Godot {spec.version}: {actual_version}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Download pinned Godot test engines.")
    parser.add_argument("--refresh", action="store_true")
    arguments = parser.parse_args()
    download_test_engines(arguments.refresh)


if __name__ == "__main__":
    main()
