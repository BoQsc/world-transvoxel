#!/usr/bin/env python3
"""Replay captured GPU cohort inputs through the native selector, without Godot."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess


def key(member: dict) -> list[int]:
    return [int(member[name]) for name in ("page_x", "page_y", "page_z", "lod")]


def snapshots(value):
    if isinstance(value, dict):
        if value.get("schema") == "world_transvoxel.gpu_publication_inspection.v1":
            yield value
        else:
            for child in value.values():
                yield from snapshots(child)
    elif isinstance(value, list):
        for child in value:
            yield from snapshots(child)


def encode(snapshot: dict) -> str:
    lines = [" ".join(map(str, key(snapshot["seed"])))]
    for name in ("candidates", "pending_retirements"):
        members = snapshot[name]
        lines.append(str(len(members)))
        lines.extend(" ".join(map(str, key(member))) for member in members)
    lines.append(str(len(snapshot["boundaries"])))
    lines.extend(" ".join(map(str, key(member) + [
        int(member["boundary_mask"]), int(member["compatible_active"]),
    ])) for member in snapshot["boundaries"])
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=pathlib.Path, required=True)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--iterations", type=int, default=50)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output exists; choose a new evidence path")
    if not 1 <= args.iterations <= 1000:
        parser.error("iterations must be between 1 and 1000")
    source = args.capture.read_bytes()
    fixtures = list(snapshots(json.loads(source)))
    if not fixtures:
        parser.error("capture has no publication inspections")
    results = []
    for snapshot in fixtures:
        if not snapshot["built"]:
            parser.error("failed builds have incomplete lookup inventories; cannot replay")
        snapshot = dict(snapshot)
        snapshot["candidates"] = snapshot["pending_replacements"] + snapshot["ready_replacements"]
        execution = subprocess.run(
            [str(args.binary.resolve()), "--replay", str(args.iterations)],
            input=encode(snapshot), text=True, capture_output=True, check=True, timeout=60,
        )
        actual = json.loads(execution.stdout)
        matches = {name: sorted(actual[name]) == sorted(key(member) for member in snapshot[name])
                   for name in ("selected", "retirements", "waiting_masks")}
        matches["built"] = actual["built"] == snapshot["built"]
        results.append({"seed": key(snapshot["seed"]), "matches_capture": matches,
                        "measurement": actual})
    passed = all(all(item["matches_capture"].values()) for item in results)
    report = {
        "schema": "world_transvoxel.gpu_publication_replay.v1",
        "claim_boundary": "Exact selector output and isolated CPU query timing only; not GPU, frame-time or gameplay acceptance.",
        "capture_sha256": hashlib.sha256(source).hexdigest(),
        "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        "passed": passed, "results": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": passed, "snapshots": len(results), "timing": [
        {"seed": item["seed"], "members": len(item["measurement"]["selected"]),
         "median_us": item["measurement"]["median_us"]} for item in results
    ]}, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
