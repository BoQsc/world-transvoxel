#!/usr/bin/env python3

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import os
import platform
import re
import subprocess
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


SAMPLE_PATTERN = re.compile(
    r"M5_WORKLOAD_BENCHMARK_SAMPLE run=(\d+) duration_ns=(\d+)"
)
PASS_PATTERN = re.compile(
    r"M5_WORKLOAD_BENCHMARK_PASS runs=(\d+) warmup=(\d+) frames_per_run=(\d+)"
)
DEFAULT_OUTPUT = REPO_ROOT / "docs" / "evidence" / "m5_runtime_budget_windows_x86_64.json"
DEFAULT_BUDGET = REPO_ROOT / "tests" / "performance" / "m5_runtime_budget.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def physical_memory_bytes() -> int | None:
    if sys.platform == "win32":
        class MemoryStatus(ctypes.Structure):
            _fields_ = [
                ("length", ctypes.c_ulong),
                ("memory_load", ctypes.c_ulong),
                ("total_physical", ctypes.c_ulonglong),
                ("available_physical", ctypes.c_ulonglong),
                ("total_page_file", ctypes.c_ulonglong),
                ("available_page_file", ctypes.c_ulonglong),
                ("total_virtual", ctypes.c_ulonglong),
                ("available_virtual", ctypes.c_ulonglong),
                ("available_extended_virtual", ctypes.c_ulonglong),
            ]

        status = MemoryStatus()
        status.length = ctypes.sizeof(status)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return int(status.total_physical)
        return None
    page_size = os.sysconf("SC_PAGE_SIZE") if hasattr(os, "sysconf") else None
    page_count = os.sysconf("SC_PHYS_PAGES") if hasattr(os, "sysconf") else None
    if isinstance(page_size, int) and isinstance(page_count, int):
        return page_size * page_count
    return None


def open_windows_process_handle(process_id: int) -> object | None:
    if sys.platform != "win32":
        return None
    process_query_limited_information = 0x1000
    process_vm_read = 0x0010
    open_process = ctypes.windll.kernel32.OpenProcess
    open_process.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    open_process.restype = ctypes.c_void_p
    handle = open_process(
        process_query_limited_information | process_vm_read,
        False,
        process_id,
    )
    return handle if handle else None


def close_windows_handle(handle: object | None) -> None:
    if handle is not None:
        close_handle = ctypes.windll.kernel32.CloseHandle
        close_handle.argtypes = [ctypes.c_void_p]
        close_handle.restype = ctypes.c_int
        close_handle(handle)


def query_windows_peak_working_set(handle: object | None) -> int | None:
    if handle is None:
        return None

    class ProcessMemoryCounters(ctypes.Structure):
        _fields_ = [
            ("cb", ctypes.c_ulong),
            ("page_fault_count", ctypes.c_ulong),
            ("peak_working_set_size", ctypes.c_size_t),
            ("working_set_size", ctypes.c_size_t),
            ("quota_peak_paged_pool_usage", ctypes.c_size_t),
            ("quota_paged_pool_usage", ctypes.c_size_t),
            ("quota_peak_non_paged_pool_usage", ctypes.c_size_t),
            ("quota_non_paged_pool_usage", ctypes.c_size_t),
            ("pagefile_usage", ctypes.c_size_t),
            ("peak_pagefile_usage", ctypes.c_size_t),
        ]

    counters = ProcessMemoryCounters()
    counters.cb = ctypes.sizeof(counters)
    get_process_memory_info = ctypes.windll.psapi.GetProcessMemoryInfo
    get_process_memory_info.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_ulong,
    ]
    get_process_memory_info.restype = ctypes.c_int
    if get_process_memory_info(
        handle,
        ctypes.byref(counters),
        counters.cb,
    ):
        return int(counters.peak_working_set_size)
    return None


def run_native_benchmark(
    executable: Path,
    iterations: int,
    warmup: int,
) -> tuple[list[int], int, int | None, str]:
    process = subprocess.Popen(
        [
            str(executable),
            "--benchmark-runs",
            str(iterations),
            "--warmup-runs",
            str(warmup),
        ],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    handle = open_windows_process_handle(process.pid)
    try:
        combined, _ = process.communicate()
        peak_working_set = query_windows_peak_working_set(handle)
    finally:
        close_windows_handle(handle)
    print(combined, end="" if combined.endswith("\n") else "\n")
    if process.returncode != 0:
        raise RuntimeError(f"M5 runtime benchmark failed with {process.returncode}.")
    samples = [
        int(match.group(2))
        for match in SAMPLE_PATTERN.finditer(combined)
    ]
    pass_match = PASS_PATTERN.search(combined)
    if (
        pass_match is None
        or int(pass_match.group(1)) != iterations
        or int(pass_match.group(2)) != warmup
        or len(samples) != iterations
    ):
        raise RuntimeError("M5 runtime benchmark output contract failed.")
    return samples, int(pass_match.group(3)), peak_working_set, combined


def percentile(samples: list[int], probability: float) -> int:
    ordered = sorted(samples)
    rank = max(1, math.ceil(probability * len(ordered)))
    return ordered[rank - 1]


def load_budget(path: Path, profile_name: str) -> dict:
    with path.open("r", encoding="utf-8") as source:
        document = json.load(source)
    if document.get("schema") != 1:
        raise RuntimeError(f"Unsupported M5 budget schema in {path}.")
    for profile in document.get("profiles", []):
        if profile.get("name") == profile_name:
            return profile
    raise RuntimeError(f"Missing M5 budget profile {profile_name!r} in {path}.")


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
    reference_host = profile.get("reference_host", {})
    mismatches = [
        f"{key}={host.get(key)!r} expected={expected!r}"
        for key, expected in reference_host.items()
        if host.get(key) != expected
    ]
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
        raise RuntimeError("M5 runtime budget exceeded: " + ", ".join(failures))


def main() -> None:
    require_supported_python()
    parser = argparse.ArgumentParser(
        description="Measure the M5 representative native runtime workload."
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
        "test_wt_m5_workload",
    )
    if not executable.is_file():
        raise RuntimeError(f"Missing M5 workload executable: {executable}")
    samples, frames_per_run, peak_working_set, _ = run_native_benchmark(
        executable,
        arguments.iterations,
        arguments.warmup,
    )
    selected_host = host_platform()
    p50 = percentile(samples, 0.50)
    p95 = percentile(samples, 0.95)
    p99 = percentile(samples, 0.99)
    result = {
        "schema": 1,
        "captured_utc": datetime.now(UTC).isoformat(),
        "scope": "synthetic_native_runtime_orchestration",
        "source": {
            "git_revision": output(["git", "rev-parse", "HEAD"]),
            "git_describe": output(["git", "describe", "--always", "--dirty"]),
            "executable": str(executable.relative_to(REPO_ROOT)).replace("\\", "/"),
            "executable_sha256": sha256(executable),
            "configuration": "template_release",
            "zig_version": output([REPO_ROOT / ".tools" / "zig" / "zig.exe", "version"])
            if sys.platform == "win32"
            else output([REPO_ROOT / ".tools" / "zig" / "zig", "version"]),
        },
        "host": {
            "platform": selected_host.godot_platform,
            "architecture": selected_host.godot_arch,
            "platform_description": platform.platform(),
            "cpu": platform.processor()
            or os.environ.get("PROCESSOR_IDENTIFIER")
            or "unknown",
            "logical_cpu_count": os.cpu_count(),
            "physical_memory_bytes": physical_memory_bytes(),
        },
        "workload": {
            "iterations": arguments.iterations,
            "warmup_iterations": arguments.warmup,
            "frames_per_run": frames_per_run,
            "viewer_events_per_run": 118,
            "edit_events_per_run": 8,
            "worker_jobs_per_run": 526,
        },
        "measurements": {
            "scenario_p50_ns": p50,
            "scenario_p95_ns": p95,
            "scenario_p99_ns": p99,
            "scenario_max_ns": max(samples),
            "simulated_frame_p95_ns": math.ceil(p95 / frames_per_run),
            "peak_working_set_bytes": peak_working_set,
        },
        "samples_ns": samples,
    }
    if not arguments.no_budget_check:
        profile = load_budget(arguments.budget, arguments.budget_profile)
        check_budget(result, profile)
        result["budget_profile"] = profile
    arguments.output = arguments.output.resolve()
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    with arguments.output.open("w", encoding="utf-8", newline="\n") as destination:
        json.dump(result, destination, indent=2, sort_keys=True)
        destination.write("\n")
    print(json.dumps(result["measurements"], sort_keys=True))
    print(f"M5_RUNTIME_BUDGET_PASS output={arguments.output}")


if __name__ == "__main__":
    main()
