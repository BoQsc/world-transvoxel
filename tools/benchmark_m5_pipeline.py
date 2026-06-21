#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
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
    percentile,
    reference_host_mismatches,
    run_process_with_peak,
    sha256,
)


SAMPLE_PATTERN = re.compile(
    r"M5_PIPELINE_BENCHMARK_SAMPLE run=(\d+) "
    r"io_decode_ns=(\d+) page_mesh_ns=(\d+) transition_mesh_ns=(\d+)"
)
PASS_PATTERN = re.compile(
    r"M5_PIPELINE_BENCHMARK_PASS runs=(\d+) warmup=(\d+) "
    r"pages_per_run=(\d+) page_mesh_chunks_per_run=(\d+) "
    r"transition_chunks_per_run=(\d+) bytes_per_run=(\d+) "
    r"page_vertices=(\d+) page_triangles=(\d+) "
    r"transition_vertices=(\d+) transition_triangles=(\d+) "
    r"backend=([a-z0-9_]+)"
)
DEFAULT_OUTPUT = (
    REPO_ROOT / "docs" / "evidence" / "m5_pipeline_budget_windows_x86_64.json"
)
DEFAULT_BUDGET = REPO_ROOT / "tests" / "performance" / "m5_pipeline_budget.json"


def measurement_summary(prefix: str, samples: list[int]) -> dict[str, int]:
    return {
        f"{prefix}_p50_ns": percentile(samples, 0.50),
        f"{prefix}_p95_ns": percentile(samples, 0.95),
        f"{prefix}_p99_ns": percentile(samples, 0.99),
        f"{prefix}_max_ns": max(samples),
    }


def run_native_benchmark(
    executable: Path,
    iterations: int,
    warmup: int,
) -> tuple[dict[str, list[int]], dict, int | None]:
    returncode, combined, peak_working_set = run_process_with_peak(
        [
            executable,
            "--benchmark-runs",
            str(iterations),
            "--warmup-runs",
            str(warmup),
        ],
        REPO_ROOT,
    )
    print(combined, end="" if combined.endswith("\n") else "\n")
    if returncode != 0:
        raise RuntimeError(f"M5 pipeline benchmark failed with {returncode}.")
    matches = list(SAMPLE_PATTERN.finditer(combined))
    pass_match = PASS_PATTERN.search(combined)
    if (
        pass_match is None
        or int(pass_match.group(1)) != iterations
        or int(pass_match.group(2)) != warmup
        or len(matches) != iterations
        or [int(match.group(1)) for match in matches]
        != list(range(1, iterations + 1))
    ):
        raise RuntimeError("M5 pipeline benchmark output contract failed.")
    samples = {
        "io_decode_batch": [int(match.group(2)) for match in matches],
        "page_mesh_batch": [int(match.group(3)) for match in matches],
        "transition_mesh_batch": [int(match.group(4)) for match in matches],
    }
    workload = {
        "iterations": iterations,
        "warmup_iterations": warmup,
        "pages_per_run": int(pass_match.group(3)),
        "page_mesh_chunks_per_run": int(pass_match.group(4)),
        "transition_chunks_per_run": int(pass_match.group(5)),
        "encoded_bytes_per_run": int(pass_match.group(6)),
        "page_mesh_vertices_per_run": int(pass_match.group(7)),
        "page_mesh_triangles_per_run": int(pass_match.group(8)),
        "transition_mesh_vertices_per_run": int(pass_match.group(9)),
        "transition_mesh_triangles_per_run": int(pass_match.group(10)),
        "backend": pass_match.group(11),
    }
    return samples, workload, peak_working_set


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
    failures = []
    for key, limit in profile["limits"].items():
        actual = result["measurements"].get(key)
        if actual is None or actual > limit:
            failures.append(f"{key}={actual} limit={limit}")
    if failures:
        raise RuntimeError("M5 pipeline budget exceeded: " + ", ".join(failures))


def main() -> None:
    require_supported_python()
    parser = argparse.ArgumentParser(
        description="Measure real page I/O/decode and MIT chunk meshing."
    )
    parser.add_argument("--iterations", type=int, default=101)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--no-budget-check", action="store_true")
    parser.add_argument("--budget", type=Path, default=DEFAULT_BUDGET)
    parser.add_argument(
        "--budget-profile",
        default="windows-x86_64-reference",
    )
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    arguments = parser.parse_args()
    if arguments.iterations < 3 or arguments.warmup < 1:
        raise RuntimeError("Use at least 3 measured and 1 warmup iteration.")
    if not arguments.skip_build:
        build("release")

    executable = native_test_path(
        "template_release",
        "test_wt_m5_pipeline_budget",
    )
    if not executable.is_file():
        raise RuntimeError(f"Missing M5 pipeline executable: {executable}")
    samples, workload, peak_working_set = run_native_benchmark(
        executable,
        arguments.iterations,
        arguments.warmup,
    )
    measurements: dict[str, int | None] = {
        "peak_working_set_bytes": peak_working_set,
    }
    for prefix, values in samples.items():
        measurements.update(measurement_summary(prefix, values))
    measurements["io_decode_per_page_p95_ns"] = -(
        -measurements["io_decode_batch_p95_ns"] // workload["pages_per_run"]
    )
    measurements["page_mesh_per_chunk_p95_ns"] = -(
        -measurements["page_mesh_batch_p95_ns"]
        // workload["page_mesh_chunks_per_run"]
    )
    measurements["transition_mesh_per_chunk_p95_ns"] = -(
        -measurements["transition_mesh_batch_p95_ns"]
        // workload["transition_chunks_per_run"]
    )

    result = {
        "schema": 1,
        "captured_utc": datetime.now(UTC).isoformat(),
        "scope": "warm_page_io_decode_and_mit_chunk_meshing",
        "source": {
            "git_revision": output(["git", "rev-parse", "HEAD"]),
            "git_describe": output(["git", "describe", "--always", "--dirty"]),
            "executable": str(executable.relative_to(REPO_ROOT)).replace("\\", "/"),
            "executable_sha256": sha256(executable),
            "configuration": "template_release",
            "zig_version": output(
                [
                    REPO_ROOT / ".tools" / "zig" / (
                        "zig.exe" if sys.platform == "win32" else "zig"
                    ),
                    "version",
                ]
            ),
        },
        "host": host_record(host_platform()),
        "workload": workload,
        "measurements": measurements,
        "samples_ns": samples,
    }
    if not arguments.no_budget_check:
        profile = load_budget(
            arguments.budget,
            arguments.budget_profile,
            "M5 pipeline",
        )
        check_budget(result, profile)
        result["budget_profile"] = profile
    arguments.output = arguments.output.resolve()
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    with arguments.output.open("w", encoding="utf-8", newline="\n") as destination:
        json.dump(result, destination, indent=2, sort_keys=True)
        destination.write("\n")
    print(json.dumps(result["measurements"], sort_keys=True))
    print(f"M5_PIPELINE_BUDGET_PASS output={arguments.output}")


if __name__ == "__main__":
    main()
