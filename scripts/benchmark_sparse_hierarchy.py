#!/usr/bin/env python3

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from wt_benchmark_common import run_process_with_peak, sha256


DEFAULT_EXECUTABLE = (
    ROOT
    / "build"
    / "native-tests"
    / "test_wt_production_snapshot_query.template_debug.x86_64.exe"
)
EXPECTED_HASH = "85405130738a0b3c38a69d1ed7f146379c7e3e470c090ff40690aec66aede7c7"
KEY_VALUE = re.compile(r"(?P<key>[a-zA-Z0-9_]+)=(?P<value>[^ ]+)")


def percentile(values: list[int], fraction: float) -> int:
    ordered = sorted(values)
    if not ordered:
        return 0
    index = int(fraction * (len(ordered) - 1))
    return ordered[index]


def distribution(values: list[int]) -> dict[str, int]:
    return {
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "worst": max(values, default=0),
        "samples": len(values),
    }


def parse_values(line: str) -> dict[str, int | str]:
    values: dict[str, int | str] = {}
    for match in KEY_VALUE.finditer(line):
        key = match.group("key")
        value = match.group("value")
        try:
            values[key] = int(value)
        except ValueError:
            values[key] = value
    return values


def mapping(value: object) -> dict[str, object]:
    if not isinstance(value, dict):
        raise RuntimeError("benchmark evidence has an invalid object shape")
    return value


def run_once(executable: Path) -> dict[str, object]:
    started_ns = time.perf_counter_ns()
    returncode, combined, peak_working_set_bytes = run_process_with_peak(
        [executable], ROOT
    )
    duration_ns = time.perf_counter_ns() - started_ns
    if returncode != 0:
        raise RuntimeError(
            f"authority test failed with exit code {returncode}\n{combined}"
        )
    lines = combined.splitlines()
    hierarchy_line = next(
        (line for line in lines if line.startswith("SPARSE_HIERARCHY_TIMINGS ")),
        "",
    )
    snapshot_line = next(
        (line for line in lines if line.startswith("SPARSE_SNAPSHOT_EVIDENCE ")),
        "",
    )
    operation_lines = [
        line for line in lines if line.startswith("SPARSE_HIERARCHY_OPERATION ")
    ]
    hash_line = next(
        (line for line in lines if line.startswith("PRODUCTION_SNAPSHOT_QUERY_HASH ")),
        "",
    )
    pass_line = next(
        (line for line in lines if line.startswith("PRODUCTION_SNAPSHOT_QUERY_PASS ")),
        "",
    )
    native_hash = hash_line.removeprefix("PRODUCTION_SNAPSHOT_QUERY_HASH ").strip()
    if not hierarchy_line or not snapshot_line or not operation_lines:
        raise RuntimeError("authority test did not emit complete sparse evidence")
    if native_hash != EXPECTED_HASH or not pass_line:
        raise RuntimeError("authority test hash or pass marker changed")
    return {
        "duration_ns": duration_ns,
        "peak_working_set_bytes": peak_working_set_bytes or 0,
        "hierarchy": parse_values(hierarchy_line),
        "hierarchy_operations": {
            str(values["operation"]): values
            for values in (parse_values(line) for line in operation_lines)
        },
        "snapshot": parse_values(snapshot_line),
        "native_hash": native_hash,
    }


def aggregate_report(runs: list[dict[str, object]]) -> dict[str, object]:
    hierarchy_fields = [
        "implicit_startup_ns",
        "flat_startup_ns",
        "implicit_p50",
        "implicit_p95",
        "implicit_p99",
        "implicit_worst",
        "flat_p50",
        "flat_p95",
        "flat_p99",
        "flat_worst",
    ]
    snapshot_fields = [
        "cold_open_ns",
        "first_edit_append_ns",
        "edit_invalidation_query_ns",
        "first_compaction_ns",
        "first_reopen_ns",
        "second_edit_append_ns",
        "second_compaction_ns",
        "final_reopen_ns",
        "migration_ns",
        "migrated_reopen_ns",
    ]
    operation_names = sorted(
        mapping(runs[0]["hierarchy_operations"]).keys()
    )
    hierarchy_summary = {
        field: distribution([
            int(mapping(run["hierarchy"])[field]) for run in runs
        ])
        for field in hierarchy_fields
    }
    snapshot_summary = {
        field: distribution([
            int(mapping(run["snapshot"])[field]) for run in runs
        ])
        for field in snapshot_fields
    }
    operation_summary = {}
    for operation in operation_names:
        operation_summary[operation] = {
            field: distribution([
                int(mapping(mapping(run["hierarchy_operations"])[operation])[field])
                for run in runs
            ])
            for field in ("p50_ns", "p95_ns", "p99_ns", "worst_ns")
        }
    fixed_fields = (
        "declared_pages",
        "overlay_pages",
        "overlay_index_bytes",
        "hierarchy_index_bytes",
    )
    fixed = {
        field: int(mapping(runs[0]["snapshot"])[field]) for field in fixed_fields
    }
    for run in runs[1:]:
        for field, expected in fixed.items():
            if int(mapping(run["snapshot"])[field]) != expected:
                raise RuntimeError(f"non-deterministic sparse counter: {field}")
    return {
        "process_duration_ns": distribution([
            int(run["duration_ns"]) for run in runs
        ]),
        "peak_working_set_bytes": distribution([
            int(run["peak_working_set_bytes"]) for run in runs
        ]),
        "hierarchy": hierarchy_summary,
        "hierarchy_operations": operation_summary,
        "snapshot_operations": snapshot_summary,
        "fixed_counters": fixed,
    }


def git_commit() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Measure the sparse hierarchy and procedural snapshot authority test."
    )
    parser.add_argument("--executable", type=Path, default=DEFAULT_EXECUTABLE)
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    executable = arguments.executable.resolve()
    if arguments.runs < 1 or arguments.warmup < 0:
        parser.error("--runs must be positive and --warmup must be non-negative")
    if not executable.is_file():
        parser.error(f"authority executable does not exist: {executable}")
    for _ in range(arguments.warmup):
        run_once(executable)
    runs = [run_once(executable) for _ in range(arguments.runs)]
    report = {
        "schema": "world_transvoxel.sparse_hierarchy_benchmark.v1",
        "status": "PASS",
        "authority": {
            "repository": "world-transvoxel",
            "git_commit": git_commit(),
            "executable": str(executable.relative_to(ROOT)).replace("\\", "/"),
            "executable_sha256": sha256(executable),
            "native_contract_hash": EXPECTED_HASH,
        },
        "method": {
            "warmup_runs": arguments.warmup,
            "measured_runs": arguments.runs,
            "hierarchy_queries_per_native_run": 4096,
            "memory_metric": "windows_peak_working_set",
        },
        "summary": aggregate_report(runs),
        "runs": runs,
    }
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(encoded, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
