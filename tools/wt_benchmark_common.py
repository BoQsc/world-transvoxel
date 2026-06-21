#!/usr/bin/env python3

from __future__ import annotations

import ctypes
import hashlib
import json
import math
import os
import platform
import subprocess
import sys
from pathlib import Path
from typing import Sequence


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
    if get_process_memory_info(handle, ctypes.byref(counters), counters.cb):
        return int(counters.peak_working_set_size)
    return None


def run_process_with_peak(
    command: Sequence[str | Path],
    cwd: Path,
) -> tuple[int, str, int | None]:
    process = subprocess.Popen(
        [str(argument) for argument in command],
        cwd=cwd,
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
    return process.returncode, combined, peak_working_set


def percentile(samples: list[int], probability: float) -> int:
    ordered = sorted(samples)
    rank = max(1, math.ceil(probability * len(ordered)))
    return ordered[rank - 1]


def load_budget(path: Path, profile_name: str, label: str) -> dict:
    with path.open("r", encoding="utf-8") as source:
        document = json.load(source)
    if document.get("schema") != 1:
        raise RuntimeError(f"Unsupported {label} budget schema in {path}.")
    for profile in document.get("profiles", []):
        if profile.get("name") == profile_name:
            return profile
    raise RuntimeError(f"Missing {label} budget profile {profile_name!r} in {path}.")


def host_record(selected_host: object) -> dict:
    return {
        "platform": selected_host.godot_platform,
        "architecture": selected_host.godot_arch,
        "platform_description": platform.platform(),
        "cpu": platform.processor()
        or os.environ.get("PROCESSOR_IDENTIFIER")
        or "unknown",
        "logical_cpu_count": os.cpu_count(),
        "physical_memory_bytes": physical_memory_bytes(),
    }


def reference_host_mismatches(host: dict, profile: dict) -> list[str]:
    return [
        f"{key}={host.get(key)!r} expected={expected!r}"
        for key, expected in profile.get("reference_host", {}).items()
        if host.get(key) != expected
    ]
