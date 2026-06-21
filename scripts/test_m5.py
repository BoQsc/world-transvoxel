#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys

from build import build
from test_m4 import test_m4
from wt_script_common import REPO_ROOT, native_test_path


EXPECTED_ASYNC_STORAGE_HASH = (
    "96ba7123c6b86fe9e2f07aa17f27553f58db92cca6427a069eced98de1471402"
)
EXPECTED_STORAGE_CACHE_HASH = (
    "11b2749ef19124bf73f6f2e287f0cc0da3c877fd9876d3cd42e0d031bd0f740f"
)
EXPECTED_RESOURCE_CACHE_HASH = (
    "842a4104d541f930c88ed3c6bcea6a1a34f83c4725b88cd3da28803a90c17dc8"
)
EXPECTED_MULTI_VIEWER_HASH = (
    "65c5397d2e6174c496a6b3ebc06d1547b39cc0c0ca36a1794e5177914c7fe696"
)
EXPECTED_EDIT_REPLACEMENT_HASH = (
    "03eedad6263963350d32226bd5c59f9aba48e4b35adefa4f2cd774cd70cfb9df"
)
EXPECTED_WORKLOAD_HASH = (
    "c5bdf6b8896f0a5e4271c5aeab2e8f552e7b776bccc66e9082595467ff90b2a3"
)


def run_hashed_test(
    configuration: str,
    test_name: str,
    pass_marker: str,
    hash_label: str,
    expected_hash: str,
) -> None:
    executable = native_test_path(configuration, test_name)
    if not executable.is_file():
        raise RuntimeError(f"Missing M5 test executable: {executable}")
    result = subprocess.run(
        [str(executable)],
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
    )
    combined = result.stdout + result.stderr
    print(combined, end="" if combined.endswith("\n") else "\n")
    match = re.search(rf"{hash_label} ([0-9a-f]{{64}})", combined)
    if (
        result.returncode != 0
        or pass_marker not in combined
        or match is None
        or match.group(1) != expected_hash
    ):
        actual = "missing" if match is None else match.group(1)
        raise RuntimeError(
            f"{test_name} contract failed for {configuration}: {actual}"
        )


def run_workload_benchmark_smoke() -> None:
    evidence = REPO_ROOT / "build" / "m5_runtime_budget_smoke.json"
    evidence.unlink(missing_ok=True)
    result = subprocess.run(
        [
            sys.executable,
            str(REPO_ROOT / "tools" / "benchmark_m5_runtime.py"),
            "--skip-build",
            "--no-budget-check",
            "--iterations",
            "3",
            "--warmup",
            "1",
            "--output",
            str(evidence),
        ],
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
    )
    combined = result.stdout + result.stderr
    print(combined, end="" if combined.endswith("\n") else "\n")
    document = (
        json.loads(evidence.read_text(encoding="utf-8"))
        if evidence.is_file()
        else {}
    )
    if (
        result.returncode != 0
        or document.get("schema") != 1
        or len(document.get("samples_ns", [])) != 3
        or "M5_RUNTIME_BUDGET_PASS" not in combined
    ):
        raise RuntimeError("M5 workload benchmark interface failed.")


def run_pipeline_benchmark_smoke() -> None:
    evidence = REPO_ROOT / "build" / "m5_pipeline_budget_smoke.json"
    evidence.unlink(missing_ok=True)
    result = subprocess.run(
        [
            sys.executable,
            str(REPO_ROOT / "tools" / "benchmark_m5_pipeline.py"),
            "--skip-build",
            "--no-budget-check",
            "--iterations",
            "3",
            "--warmup",
            "1",
            "--output",
            str(evidence),
        ],
        check=False,
        text=True,
        capture_output=True,
        errors="replace",
    )
    combined = result.stdout + result.stderr
    print(combined, end="" if combined.endswith("\n") else "\n")
    document = (
        json.loads(evidence.read_text(encoding="utf-8"))
        if evidence.is_file()
        else {}
    )
    samples = document.get("samples_ns", {})
    if (
        result.returncode != 0
        or document.get("schema") != 1
        or any(
            len(samples.get(key, [])) != 3
            for key in (
                "io_decode_batch",
                "page_mesh_batch",
                "transition_mesh_batch",
            )
        )
        or "M5_PIPELINE_BUDGET_PASS" not in combined
    ):
        raise RuntimeError("M5 pipeline benchmark interface failed.")


def test_m5(
    skip_build: bool = False,
    skip_engine_download: bool = False,
) -> None:
    if not skip_build:
        build("all")
    for configuration in ("template_debug", "template_release"):
        run_hashed_test(
            configuration,
            "test_wt_m5_async_storage",
            "M5_ASYNC_STORAGE_PASS",
            "M5_ASYNC_STORAGE_HASH",
            EXPECTED_ASYNC_STORAGE_HASH,
        )
        run_hashed_test(
            configuration,
            "test_wt_m5_storage_cache",
            "M5_STORAGE_CACHE_PASS",
            "M5_STORAGE_CACHE_HASH",
            EXPECTED_STORAGE_CACHE_HASH,
        )
        run_hashed_test(
            configuration,
            "test_wt_m5_resource_cache",
            "M5_RESOURCE_CACHE_PASS",
            "M5_RESOURCE_CACHE_HASH",
            EXPECTED_RESOURCE_CACHE_HASH,
        )
        run_hashed_test(
            configuration,
            "test_wt_m5_multi_viewer",
            "M5_MULTI_VIEWER_PASS",
            "M5_MULTI_VIEWER_HASH",
            EXPECTED_MULTI_VIEWER_HASH,
        )
        run_hashed_test(
            configuration,
            "test_wt_m5_edit_replacement",
            "M5_EDIT_REPLACEMENT_PASS",
            "M5_EDIT_REPLACEMENT_HASH",
            EXPECTED_EDIT_REPLACEMENT_HASH,
        )
        run_hashed_test(
            configuration,
            "test_wt_m5_workload",
            "M5_WORKLOAD_PASS",
            "M5_WORKLOAD_HASH",
            EXPECTED_WORKLOAD_HASH,
        )
    run_workload_benchmark_smoke()
    run_pipeline_benchmark_smoke()
    test_m4(skip_build=True, skip_engine_download=skip_engine_download)
    print(
        "M5 storage, cache, multi-viewer, edit replacement, and representative "
        "functional workloads plus runtime and pipeline budget interfaces "
        "passed with the complete M4 suite."
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run the active M5 validation suite."
    )
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-engine-download", action="store_true")
    arguments = parser.parse_args()
    test_m5(arguments.skip_build, arguments.skip_engine_download)


if __name__ == "__main__":
    main()
