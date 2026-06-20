#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from pathlib import Path

from wt_script_common import REPO_ROOT, download_file, output, run, sha256


DOWNLOAD_ROOT = REPO_ROOT / "references" / "downloaded"
MANIFEST_PATH = REPO_ROOT / "references" / "manifest.json"
LOCK_PATH = REPO_ROOT / "references" / "lock.json"


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as source:
        return json.load(source)


def sync_repository(url: str, revision: str, target: Path) -> None:
    if not (target / ".git").exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--filter=blob:none", "--no-checkout", url, target])
    run(["git", "fetch", "--depth", "1", "origin", revision], cwd=target)
    run(["git", "checkout", "--detach", revision], cwd=target)
    actual_revision = output(["git", "rev-parse", "HEAD"], cwd=target)
    if actual_revision != revision:
        raise RuntimeError(
            f"Revision mismatch for {target.name}: expected {revision}, got {actual_revision}"
        )


def download_references(refresh: bool = False) -> None:
    manifest = load_json(MANIFEST_PATH)
    lock = load_json(LOCK_PATH)
    locked_papers = {entry["target"]: entry for entry in lock["papers"]}

    for paper in manifest["papers"]:
        target = DOWNLOAD_ROOT / paper["target"]
        download_file(paper["url"], target, refresh)
        expected = locked_papers[paper["target"]]
        actual_hash = sha256(target)
        actual_size = target.stat().st_size
        if actual_hash != expected["sha256"] or actual_size != expected["bytes"]:
            raise RuntimeError(
                f"Paper lock mismatch for {paper['target']}: "
                f"size={actual_size}, sha256={actual_hash}"
            )

    for repository in manifest["repositories"]:
        sync_repository(
            repository["url"],
            repository["revision"],
            DOWNLOAD_ROOT / repository["target"],
        )

    print("\nDownloaded paper hashes:")
    for path in sorted((DOWNLOAD_ROOT / "papers").iterdir(), key=lambda item: item.name):
        if path.is_file():
            print(f"{sha256(path)}  {path.name}")

    print("\nPinned repository revisions:")
    upstream_root = DOWNLOAD_ROOT / "upstream"
    for path in sorted(upstream_root.iterdir(), key=lambda item: item.name):
        if path.is_dir() and (path / ".git").exists():
            print(f"{path.name} {output(['git', 'rev-parse', 'HEAD'], cwd=path)}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Download and verify pinned references.")
    parser.add_argument("--refresh", action="store_true")
    arguments = parser.parse_args()
    download_references(arguments.refresh)


if __name__ == "__main__":
    main()
