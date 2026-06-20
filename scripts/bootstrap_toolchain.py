#!/usr/bin/env python3

from __future__ import annotations

import argparse
import shutil
import sys

from wt_script_common import (
    DOWNLOAD_ROOT,
    REPO_ROOT,
    TOOLS_ROOT,
    download_file,
    extract_archive,
    host_platform,
    output,
    remove_tree,
    require_supported_python,
    run,
    sha256,
)


ZIG_VERSION = "0.16.0"
GODOT_CPP_REVISION = "e83fd0904c13356ed1d4c3d09f8bb9132bdc6b77"
ZIG_ARCHIVES = {
    "x86_64-windows": ("zip", "68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e"),
    "aarch64-windows": ("zip", "aee38316ee4111717900f45dd3130145c39289e105541d737eb8c5ed653c78ef"),
    "x86_64-linux": ("tar.xz", "70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00"),
    "aarch64-linux": ("tar.xz", "ea4b09bfb22ec6f6c6ceac57ab63efb6b46e17ab08d21f69f3a48b38e1534f17"),
    "x86_64-macos": ("tar.xz", "0387557ed1877bc6a2e1802c8391953baddba76081876301c522f52977b52ba7"),
    "aarch64-macos": ("tar.xz", "b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489"),
}


def bootstrap(refresh: bool = False) -> None:
    require_supported_python()
    host = host_platform()
    archive_type, expected_hash = ZIG_ARCHIVES[host.zig_target]
    archive_name = f"zig-{host.zig_target}-{ZIG_VERSION}.{archive_type}"
    archive_path = DOWNLOAD_ROOT / archive_name
    archive_url = f"https://ziglang.org/download/{ZIG_VERSION}/{archive_name}"
    zig_root = TOOLS_ROOT / "zig"
    zig_executable = zig_root / f"zig{host.executable_suffix}"

    DOWNLOAD_ROOT.mkdir(parents=True, exist_ok=True)
    download_file(archive_url, archive_path, refresh)
    actual_hash = sha256(archive_path)
    if actual_hash != expected_hash:
        raise RuntimeError(
            f"Zig archive hash mismatch: expected {expected_hash}, got {actual_hash}"
        )

    if refresh or not zig_executable.is_file():
        stage_root = TOOLS_ROOT / "zig-extract-stage"
        remove_tree(stage_root, TOOLS_ROOT)
        remove_tree(zig_root, TOOLS_ROOT)
        extract_archive(archive_path, stage_root)
        extracted_root = stage_root / f"zig-{host.zig_target}-{ZIG_VERSION}"
        if not extracted_root.is_dir():
            raise RuntimeError(f"Zig archive root is missing: {extracted_root}")
        shutil.move(extracted_root, zig_root)
        remove_tree(stage_root, TOOLS_ROOT)

    actual_zig_version = output([zig_executable, "version"])
    if actual_zig_version != ZIG_VERSION:
        raise RuntimeError(
            f"Expected Zig {ZIG_VERSION}, got {actual_zig_version or '<empty>'}"
        )

    godot_cpp_root = REPO_ROOT / "thirdparty" / "godot-cpp"
    if not (godot_cpp_root / ".git").exists():
        run(
            [
                "git",
                "-C",
                REPO_ROOT,
                "submodule",
                "update",
                "--init",
                "--recursive",
                "thirdparty/godot-cpp",
            ]
        )
    actual_revision = output(["git", "rev-parse", "HEAD"], cwd=godot_cpp_root)
    if actual_revision != GODOT_CPP_REVISION:
        raise RuntimeError(
            "godot-cpp revision mismatch: "
            f"expected {GODOT_CPP_REVISION}, got {actual_revision}"
        )

    print("Toolchain ready:")
    print(f"  Python    {'.'.join(map(str, sys.version_info[:3]))}")
    print(f"  Zig       {actual_zig_version}")
    print(f"  godot-cpp {actual_revision}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Bootstrap the pinned native toolchain.")
    parser.add_argument("--refresh", action="store_true", help="Redownload and reextract Zig.")
    arguments = parser.parse_args()
    bootstrap(arguments.refresh)


if __name__ == "__main__":
    main()
