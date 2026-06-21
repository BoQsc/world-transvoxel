#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from datetime import UTC, datetime
from pathlib import Path

SCRIPT_ROOT = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPT_ROOT))

from build import build  # noqa: E402
from download_test_engines import engine_executable, engine_specs  # noqa: E402
from wt_script_common import (  # noqa: E402
    REPO_ROOT,
    addon_binary_path,
    host_platform,
    output,
    require_supported_python,
)
from wt_benchmark_common import (  # noqa: E402
    host_record,
    load_budget,
    percentile,
    reference_host_mismatches,
    run_process_with_peak,
    sha256,
)


SAMPLE_PATTERN = re.compile(
    r"M5_GODOT_APPLICATION_SAMPLE run=(\d+) scenario_ns=(\d+) "
    r"frame_max_ns=(\d+) render_sink_ns=(\d+) collision_sink_ns=(\d+)"
)
COLD_PATTERN = re.compile(
    r"M5_GODOT_APPLICATION_COLD duration_ns=(\d+) frame_max_ns=(\d+)"
)
CLEAR_PATTERN = re.compile(r"M5_GODOT_APPLICATION_CLEAR duration_ns=(\d+)")
PASS_PATTERN = re.compile(
    r"M5_GODOT_APPLICATION_PASS runs=(\d+) warmup=(\d+) "
    r"frames_per_run=(\d+) render_count=(\d+) collision_count=(\d+) "
    r"render_budget=(\d+) collision_budget=(\d+) readiness_max=(\d+)"
)
DEFAULT_BUDGET = (
    REPO_ROOT / "tests" / "performance" / "m5_application_budget.json"
)
BENCHMARK_SCRIPT = REPO_ROOT / "tests" / "godot" / "m5_application_budget.gd"


def selected_engine(version: str):
    for spec in engine_specs():
        if spec.version == version:
            return spec, engine_executable(spec)
    raise RuntimeError(f"Unsupported configured Godot benchmark version: {version}")


def run_godot_benchmark(
    engine: Path,
    iterations: int,
    warmup: int,
) -> tuple[dict[str, list[int]], dict[str, int], int | None]:
    returncode, combined, peak_working_set = run_process_with_peak(
        [
            engine,
            "--headless",
            "--path",
            REPO_ROOT,
            "--script",
            "res://tests/godot/m5_application_budget.gd",
            "--",
            "--iterations",
            str(iterations),
            "--warmup",
            str(warmup),
        ],
        REPO_ROOT,
    )
    print(combined, end="" if combined.endswith("\n") else "\n")
    if returncode != 0:
        raise RuntimeError(
            f"M5 Godot application benchmark failed with {returncode}."
        )
    samples = {
        "scenario": [],
        "frame_max": [],
        "render_sink": [],
        "collision_sink": [],
    }
    for match in SAMPLE_PATTERN.finditer(combined):
        samples["scenario"].append(int(match.group(2)))
        samples["frame_max"].append(int(match.group(3)))
        samples["render_sink"].append(int(match.group(4)))
        samples["collision_sink"].append(int(match.group(5)))
    cold = COLD_PATTERN.search(combined)
    clear = CLEAR_PATTERN.search(combined)
    passed = PASS_PATTERN.search(combined)
    if (
        cold is None
        or clear is None
        or passed is None
        or int(passed.group(1)) != iterations
        or int(passed.group(2)) != warmup
        or any(len(values) != iterations for values in samples.values())
    ):
        raise RuntimeError("M5 Godot application benchmark output contract failed.")
    workload = {
        "frames_per_run": int(passed.group(3)),
        "render_count": int(passed.group(4)),
        "collision_count": int(passed.group(5)),
        "render_budget": int(passed.group(6)),
        "collision_budget": int(passed.group(7)),
        "readiness_max_frames": int(passed.group(8)),
        "cold_scenario_ns": int(cold.group(1)),
        "cold_frame_max_ns": int(cold.group(2)),
        "clear_ns": int(clear.group(1)),
    }
    return samples, workload, peak_working_set


def check_budget(result: dict, profile: dict) -> None:
    host = result["host"]
    if (
        host["platform"] != profile["platform"]
        or host["architecture"] != profile["architecture"]
        or result["source"]["godot_version"] != profile["godot_version"]
    ):
        raise RuntimeError("M5 Godot application budget profile mismatch.")
    mismatches = reference_host_mismatches(host, profile)
    if mismatches:
        raise RuntimeError(
            "Reference hardware mismatch; use --no-budget-check or a matching "
            "profile: " + ", ".join(mismatches)
        )
    failures = []
    for key, limit in profile["limits"].items():
        actual = result["measurements"][key]
        if actual is None or actual > limit:
            failures.append(f"{key}={actual} limit={limit}")
    if failures:
        raise RuntimeError(
            "M5 Godot application budget exceeded: " + ", ".join(failures)
        )


def default_output(version: str) -> Path:
    normalized = version.replace(".", "_")
    return (
        REPO_ROOT
        / "docs"
        / "evidence"
        / f"m5_application_budget_godot_{normalized}_windows_x86_64.json"
    )


def main() -> None:
    require_supported_python()
    parser = argparse.ArgumentParser(
        description="Measure real Godot render/physics application cost."
    )
    parser.add_argument("--engine-version", default="4.6.3")
    parser.add_argument("--iterations", type=int, default=101)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--no-budget-check", action="store_true")
    parser.add_argument("--budget", type=Path, default=DEFAULT_BUDGET)
    parser.add_argument("--budget-profile")
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    if arguments.iterations < 3 or arguments.warmup < 1:
        raise RuntimeError("Use at least 3 measured and 1 warmup iteration.")
    if not arguments.skip_build:
        build("release")

    spec, engine = selected_engine(arguments.engine_version)
    if not engine.is_file():
        raise RuntimeError(f"Required test engine is missing: {engine}")
    release_binary = addon_binary_path("template_release")
    debug_binary = addon_binary_path("template_debug")
    if not release_binary.is_file() or not debug_binary.is_file():
        raise RuntimeError("Required release/debug addon binaries are missing.")
    backup = REPO_ROOT / "build" / f"{debug_binary.name}.application-backup"
    backup.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(debug_binary, backup)
    try:
        shutil.copy2(release_binary, debug_binary)
        samples, workload, peak_working_set = run_godot_benchmark(
            engine,
            arguments.iterations,
            arguments.warmup,
        )
    finally:
        shutil.copy2(backup, debug_binary)
        backup.unlink(missing_ok=True)

    selected_host = host_platform()
    measurements = {
        "cold_scenario_ns": workload["cold_scenario_ns"],
        "cold_frame_max_ns": workload["cold_frame_max_ns"],
        "scenario_p50_ns": percentile(samples["scenario"], 0.50),
        "scenario_p95_ns": percentile(samples["scenario"], 0.95),
        "scenario_p99_ns": percentile(samples["scenario"], 0.99),
        "scenario_max_ns": max(samples["scenario"]),
        "frame_max_p50_ns": percentile(samples["frame_max"], 0.50),
        "frame_max_p95_ns": percentile(samples["frame_max"], 0.95),
        "frame_max_p99_ns": percentile(samples["frame_max"], 0.99),
        "render_sink_p50_ns": percentile(samples["render_sink"], 0.50),
        "render_sink_p95_ns": percentile(samples["render_sink"], 0.95),
        "collision_sink_p50_ns": percentile(samples["collision_sink"], 0.50),
        "collision_sink_p95_ns": percentile(samples["collision_sink"], 0.95),
        "clear_ns": workload["clear_ns"],
        "readiness_max_frames": workload["readiness_max_frames"],
        "peak_working_set_bytes": peak_working_set,
    }
    result = {
        "schema": 1,
        "captured_utc": datetime.now(UTC).isoformat(),
        "scope": "godot_main_thread_render_physics_application",
        "source": {
            "git_revision": output(["git", "rev-parse", "HEAD"]),
            "git_describe": output(["git", "describe", "--always", "--dirty"]),
            "configuration": "template_release",
            "addon_binary": str(release_binary.relative_to(REPO_ROOT)).replace(
                "\\", "/"
            ),
            "addon_binary_sha256": sha256(release_binary),
            "benchmark_script": str(BENCHMARK_SCRIPT.relative_to(REPO_ROOT)).replace(
                "\\", "/"
            ),
            "benchmark_script_sha256": sha256(BENCHMARK_SCRIPT),
            "godot_version": spec.version,
            "godot_tag": spec.tag,
            "godot_executable_sha256": sha256(engine),
        },
        "host": host_record(selected_host),
        "workload": {
            "iterations": arguments.iterations,
            "warmup_iterations": arguments.warmup,
            "frames_per_run": workload["frames_per_run"],
            "render_count": workload["render_count"],
            "collision_count": workload["collision_count"],
            "render_budget_per_frame": workload["render_budget"],
            "collision_budget_per_frame": workload["collision_budget"],
        },
        "measurements": measurements,
        "samples_ns": samples,
    }
    if not arguments.no_budget_check:
        profile_name = arguments.budget_profile or (
            f"windows-x86_64-godot-{spec.version}-reference"
        )
        profile = load_budget(
            arguments.budget,
            profile_name,
            "M5 Godot application",
        )
        check_budget(result, profile)
        result["budget_profile"] = profile
    output_path = (arguments.output or default_output(spec.version)).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="\n") as destination:
        json.dump(result, destination, indent=2, sort_keys=True)
        destination.write("\n")
    print(json.dumps(measurements, sort_keys=True))
    print(f"M5_GODOT_APPLICATION_BUDGET_PASS output={output_path}")


if __name__ == "__main__":
    main()
