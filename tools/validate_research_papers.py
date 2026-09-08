#!/usr/bin/env python3
"""Verify preserved research PDFs against references/manifest.json."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "references" / "manifest.json"


def main() -> int:
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    failures: list[str] = []
    checked = 0

    for paper in data["papers"]:
        if paper["preservation_status"] != "vendored":
            continue

        checked += 1
        path = ROOT / paper["preserved_path"]
        if not path.is_file():
            failures.append(f"missing: {paper['preserved_path']}")
            continue

        content = path.read_bytes()
        actual_digest = hashlib.sha256(content).hexdigest()
        if len(content) != paper["bytes"]:
            failures.append(
                f"size mismatch: {paper['preserved_path']} "
                f"({len(content)} != {paper['bytes']})"
            )
        if actual_digest != paper["sha256"]:
            failures.append(
                f"SHA-256 mismatch: {paper['preserved_path']} "
                f"({actual_digest} != {paper['sha256']})"
            )

    if failures:
        print("Research paper preservation validation failed:")
        for failure in failures:
            print(f"- {failure}")
        return 1

    print(f"Validated {checked} preserved research PDFs.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
