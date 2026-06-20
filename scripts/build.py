#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import sys

from bootstrap_toolchain import ZIG_VERSION, bootstrap
from wt_script_common import REPO_ROOT, host_platform, require_supported_python, run


TARGETS = {
    "debug": ("template_debug",),
    "release": ("template_release",),
    "all": ("template_debug", "template_release"),
}


def build(configuration: str = "all", jobs: int = 0, skip_bootstrap: bool = False) -> None:
    require_supported_python()
    if not skip_bootstrap:
        bootstrap()
    if jobs <= 0:
        jobs = max(1, os.cpu_count() or 1)
    host = host_platform()
    for target in TARGETS[configuration]:
        print(
            f"Building World Transvoxel {target} for "
            f"{host.godot_platform}/{host.godot_arch} with Zig {ZIG_VERSION}",
            flush=True,
        )
        run(
            [
                sys.executable,
                "-m",
                "SCons",
                "-Q",
                f"-j{jobs}",
                f"target={target}",
                f"platform={host.godot_platform}",
                f"arch={host.godot_arch}",
                "build_profile=build_profiles/m0.json",
            ],
            cwd=REPO_ROOT,
        )


def main() -> None:
    parser = argparse.ArgumentParser(description="Build World Transvoxel with Zig and SCons.")
    parser.add_argument("--configuration", choices=TARGETS, default="all")
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument("--skip-bootstrap", action="store_true")
    arguments = parser.parse_args()
    build(arguments.configuration, arguments.jobs, arguments.skip_bootstrap)


if __name__ == "__main__":
    main()
