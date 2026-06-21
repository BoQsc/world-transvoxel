#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import sys
from datetime import UTC, datetime
from pathlib import Path

SCRIPT_ROOT = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPT_ROOT))

from build import build  # noqa: E402
from wt_script_common import (  # noqa: E402
    REPO_ROOT,
    host_platform,
    native_test_path,
    output,
    require_supported_python,
)
from wt_benchmark_common import (  # noqa: E402
    host_record,
    load_budget,
    reference_host_mismatches,
    run_process_with_peak,
    sha256,
)


DEFAULT_OUTPUT = REPO_ROOT / "docs" / "evidence" / "m5_soak_windows_x86_64.json"
DEFAULT_TRACE = (
    REPO_ROOT / "docs" / "evidence" / "m5_soak_windows_x86_64.wttrace"
)
DEFAULT_BUDGET = REPO_ROOT / "tests" / "performance" / "m5_soak_budget.json"
METRICS_PREFIX = "M5_SOAK_METRICS "


def parse_metrics(combined: str) -> dict[str, int]:
    line = next(
        (item for item in combined.splitlines() if item.startswith(METRICS_PREFIX)),
        None,
    )
    if line is None:
        raise RuntimeError("M5 soak metrics output is missing.")
    metrics: dict[str, int] = {}
    for item in line[len(METRICS_PREFIX) :].split():
        key, separator, value = item.partition("=")
        if not separator or not value.isdigit():
            raise RuntimeError(f"Invalid M5 soak metric: {item!r}")
        metrics[key] = int(value)
    required = {
        "duration_ns",
        "target_ns",
        "frames",
        "viewer_events",
        "edit_events",
        "worker_jobs",
        "desired_max",
        "records_max",
        "jobs_max",
        "completions_max",
        "render_max",
        "collision_max",
        "resources_max",
        "pending_max",
        "readiness_max",
        "stale",
        "cancellations",
        "applied_render",
        "applied_collision",
        "rejections",
        "frame_max_ns",
        "trace_events",
        "trace_bytes",
    }
    missing = sorted(required - metrics.keys())
    if missing:
        raise RuntimeError("M5 soak metrics are incomplete: " + ", ".join(missing))
    return metrics


def check_budget(result: dict, profile: dict) -> None:
    host = result["host"]
    if (
        host["platform"] != profile["platform"]
        or host["architecture"] != profile["architecture"]
    ):
        raise RuntimeError(
            "Budget profile platform mismatch: "
            f"{host['platform']}/{host['architecture']}"
        )
    mismatches = reference_host_mismatches(host, profile)
    if mismatches:
        raise RuntimeError(
            "Reference hardware mismatch; use --no-budget-check or a matching "
            "profile: " + ", ".join(mismatches)
        )
    measurements = result["measurements"]
    failures = []
    for limit_name, limit in profile["limits"].items():
        if limit_name.startswith("minimum_"):
            measurement_name = limit_name.removeprefix("minimum_")
            actual = measurements.get(measurement_name)
            if actual is None or actual < limit:
                failures.append(f"{measurement_name}={actual} minimum={limit}")
        elif limit_name.startswith("maximum_"):
            measurement_name = limit_name.removeprefix("maximum_")
            actual = measurements.get(measurement_name)
            if actual is None or actual > limit:
                failures.append(f"{measurement_name}={actual} maximum={limit}")
        else:
            raise RuntimeError(f"Invalid M5 soak limit name: {limit_name}")
    if failures:
        raise RuntimeError("M5 soak budget exceeded: " + ", ".join(failures))


def main() -> None:
    require_supported_python()
    parser = argparse.ArgumentParser(
        description="Run and record the fixed-duration M5 runtime soak."
    )
    parser.add_argument("--duration-seconds", type=float, default=60.0)
    parser.add_argument("--sample-period-frames", type=int, default=1024)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--no-budget-check", action="store_true")
    parser.add_argument("--budget", type=Path, default=DEFAULT_BUDGET)
    parser.add_argument(
        "--budget-profile",
        default="windows-x86_64-reference",
    )
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--trace", type=Path, default=DEFAULT_TRACE)
    arguments = parser.parse_args()
    duration_ms = round(arguments.duration_seconds * 1000.0)
    if duration_ms < 1 or duration_ms > 3_600_000:
        raise RuntimeError("Duration must be between 0.001 and 3600 seconds.")
    if arguments.sample_period_frames < 1:
        raise RuntimeError("Sample period must be at least one frame.")
    if not arguments.skip_build:
        build("release")

    executable = native_test_path("template_release", "test_wt_m5_soak")
    if not executable.is_file():
        raise RuntimeError(f"Missing M5 soak executable: {executable}")
    arguments.trace = arguments.trace.resolve()
    arguments.trace.parent.mkdir(parents=True, exist_ok=True)
    arguments.trace.unlink(missing_ok=True)
    returncode, combined, peak_working_set = run_process_with_peak(
        [
            executable,
            "--duration-ms",
            str(duration_ms),
            "--sample-period-frames",
            str(arguments.sample_period_frames),
            "--trace",
            arguments.trace,
        ],
        REPO_ROOT,
    )
    print(combined, end="" if combined.endswith("\n") else "\n")
    if returncode != 0 or "M5_SOAK_PASS" not in combined:
        raise RuntimeError(f"M5 soak failed with {returncode}.")
    metrics = parse_metrics(combined)
    if not arguments.trace.is_file():
        raise RuntimeError("M5 soak trace was not created.")
    if metrics["trace_bytes"] != arguments.trace.stat().st_size:
        raise RuntimeError("M5 soak trace size does not match native output.")

    selected_host = host_platform()
    measurements = dict(metrics)
    measurements["peak_working_set_bytes"] = peak_working_set
    result = {
        "schema": 1,
        "captured_utc": datetime.now(UTC).isoformat(),
        "scope": "fixed_duration_native_runtime_orchestration",
        "source": {
            "git_revision": output(["git", "rev-parse", "HEAD"]),
            "git_describe": output(["git", "describe", "--always", "--dirty"]),
            "executable": str(executable.relative_to(REPO_ROOT)).replace("\\", "/"),
            "executable_sha256": sha256(executable),
            "configuration": "template_release",
            "zig_version": output(
                [
                    REPO_ROOT
                    / ".tools"
                    / "zig"
                    / ("zig.exe" if sys.platform == "win32" else "zig"),
                    "version",
                ]
            ),
        },
        "host": host_record(selected_host),
        "workload": {
            "requested_duration_ms": duration_ms,
            "sample_period_frames": arguments.sample_period_frames,
            "pattern_revision": 1,
            "trace_schema": 1,
            "trace_event_size": 128,
        },
        "trace": {
            "path": str(arguments.trace.relative_to(REPO_ROOT)).replace("\\", "/"),
            "sha256": sha256(arguments.trace),
            "bytes": arguments.trace.stat().st_size,
        },
        "measurements": measurements,
    }
    if not arguments.no_budget_check:
        profile = load_budget(
            arguments.budget,
            arguments.budget_profile,
            "M5 soak",
        )
        check_budget(result, profile)
        result["budget_profile"] = profile
    arguments.output = arguments.output.resolve()
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    with arguments.output.open("w", encoding="utf-8", newline="\n") as destination:
        json.dump(result, destination, indent=2, sort_keys=True)
        destination.write("\n")
    print(json.dumps(measurements, sort_keys=True))
    print(f"M5_SOAK_BUDGET_PASS output={arguments.output}")


if __name__ == "__main__":
    main()
