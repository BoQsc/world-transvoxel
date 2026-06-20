#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOWNLOAD_ROOT = ROOT / "references" / "downloaded"
ADDON_ROOT = ROOT / "addons" / "world_transvoxel"
GODOT_CPP_REVISION = "e83fd0904c13356ed1d4c3d09f8bb9132bdc6b77"
TRANSVOXEL_CPP_SHA256 = (
    "83a5511346b54c42e4e66dec916d3971c92f4fbda1c7878cbad5901a820dcab4"
)

REQUIRED_FILES = (
    ".gitignore",
    "LICENSE",
    "LICENSE_SCOPE.md",
    "LICENSES/MIT-Transvoxel.txt",
    "README.md",
    "IMPLEMENTATION_CHARTER.md",
    "SConstruct",
    "project.godot",
    "build_profiles/m0.json",
    "scripts/bootstrap_toolchain.ps1",
    "scripts/build.ps1",
    "scripts/download_test_engines.ps1",
    "scripts/test_m0.ps1",
    "scripts/test_m1.ps1",
    "tools/normalize_pe_timestamp.py",
    "tests/godot/addon_load_test.gd",
    "tests/godot/addon_load_test.gd.uid",
    "addons/world_transvoxel/LICENSE_SCOPE.md",
    "addons/world_transvoxel/README.md",
    "addons/world_transvoxel/plugin.cfg",
    "addons/world_transvoxel/world_transvoxel.gdextension",
    "addons/world_transvoxel/editor/world_transvoxel_plugin.gd",
    "addons/world_transvoxel/src/register_types.cpp",
    "addons/world_transvoxel/src/register_types.h",
    "addons/world_transvoxel/src/api/world_transvoxel_terrain.cpp",
    "addons/world_transvoxel/src/api/world_transvoxel_terrain.h",
    "addons/world_transvoxel/src/backend/wt_meshing_backend.h",
    "addons/world_transvoxel/src/backend/wt_cell_types.cpp",
    "addons/world_transvoxel/src/backend/wt_cell_types.h",
    "addons/world_transvoxel/src/backend/wt_transvoxel_mit_backend.cpp",
    "addons/world_transvoxel/src/backend/wt_transvoxel_mit_backend.h",
    "addons/world_transvoxel/src/core/wt_version.h",
    "addons/world_transvoxel/src/core/wt_meshing_limits.h",
    "addons/world_transvoxel/thirdparty/transvoxel_mit/LICENSE",
    "addons/world_transvoxel/thirdparty/transvoxel_mit/Transvoxel.cpp",
    "addons/world_transvoxel/thirdparty/transvoxel_mit/UPSTREAM.md",
    "docs/ROADMAP.md",
    "docs/milestones/M0_STATUS.md",
    "docs/milestones/M1_STATUS.md",
    "docs/contracts/M1_CELL_BACKEND.md",
    "docs/architecture/API_BOUNDARIES.md",
    "docs/architecture/ARCHITECTURE.md",
    "docs/architecture/BINARY_FORMATS.md",
    "docs/architecture/CODING_STANDARDS.md",
    "docs/research/MARCHING_CUBES_POSTMORTEM.md",
    "docs/research/REFERENCE_ASSESSMENT.md",
    "docs/research/VOXEL_TOOLS_FINDINGS.md",
    "references/manifest.json",
    "references/lock.json",
    "tests/native/test_wt_m1_cell_backend.cpp",
)

M0_BINARIES = (
    "addons/world_transvoxel/bin/"
    "world_transvoxel.windows.template_debug.x86_64.dll",
    "addons/world_transvoxel/bin/"
    "world_transvoxel.windows.template_release.x86_64.dll",
)

SOURCE_LIMITS = {
    ".cpp": (600, 900),
    ".cc": (600, 900),
    ".cxx": (600, 900),
    ".h": (300, 450),
    ".hpp": (300, 450),
    ".gd": (250, 400),
    ".glsl": (400, 650),
    ".gdshader": (400, 650),
}

CHARTER_HEADINGS = (
    "## 2. Product goal",
    "## 3. Production strategy and backend policy",
    "## 4. License model",
    "## 5. Repository and addon boundaries",
    "## 6. Technology baseline",
    "## 8. Runtime architecture",
    "## 12. Public API direction",
    "## 13. Binary data strategy",
    "## 14. Performance policy",
    "## 16. Testing strategy",
    "## 18. Scope control",
    "## 20. Finite implementation roadmap",
    "## 21. Controlled open decisions",
    "## 23. Immediate next work",
    "## 24. Final definition of success",
)


def fail(errors: list[str], message: str) -> None:
    errors.append(message)


def load_json(relative_path: str) -> dict:
    with (ROOT / relative_path).open("r", encoding="utf-8") as handle:
        return json.load(handle)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def pe_timestamp(path: Path) -> int:
    with path.open("rb") as binary:
        if binary.read(2) != b"MZ":
            raise ValueError("missing MZ signature")
        binary.seek(0x3C)
        pe_offset_data = binary.read(4)
        if len(pe_offset_data) != 4:
            raise ValueError("truncated DOS header")
        pe_offset = struct.unpack("<I", pe_offset_data)[0]
        binary.seek(pe_offset)
        if binary.read(4) != b"PE\0\0":
            raise ValueError("missing PE signature")
        binary.seek(pe_offset + 8)
        timestamp_data = binary.read(4)
        if len(timestamp_data) != 4:
            raise ValueError("truncated COFF header")
        return struct.unpack("<I", timestamp_data)[0]


def git_output(*arguments: str, cwd: Path = ROOT) -> str:
    result = subprocess.run(
        ("git", *arguments),
        cwd=cwd,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def validate_required_files(errors: list[str]) -> None:
    for relative_path in REQUIRED_FILES:
        if not (ROOT / relative_path).is_file():
            fail(errors, f"missing required file: {relative_path}")


def validate_charter(errors: list[str]) -> None:
    charter_path = ROOT / "IMPLEMENTATION_CHARTER.md"
    readme_path = ROOT / "README.md"
    if not charter_path.is_file() or not readme_path.is_file():
        return

    charter = charter_path.read_text(encoding="utf-8")
    readme = readme_path.read_text(encoding="utf-8")
    normalized_charter = " ".join(charter.split())

    if "[IMPLEMENTATION_CHARTER.md](IMPLEMENTATION_CHARTER.md)" not in readme:
        fail(errors, "README does not link to the canonical implementation charter")

    for heading in CHARTER_HEADINGS:
        if heading not in charter:
            fail(errors, f"implementation charter is missing section: {heading}")

    required_statements = (
        "This document is the single source of truth",
        "The next and only active milestone is M2.",
        "exact official compatibility is not proven",
    )
    for statement in required_statements:
        if statement not in normalized_charter:
            fail(errors, f"implementation charter is missing policy: {statement}")


def validate_reference_sets(errors: list[str]) -> None:
    manifest = load_json("references/manifest.json")
    lock = load_json("references/lock.json")

    if manifest.get("schema") != "world-transvoxel.references.v1":
        fail(errors, "unexpected reference manifest schema")
    if lock.get("schema") != "world-transvoxel.reference-lock.v1":
        fail(errors, "unexpected reference lock schema")

    manifest_papers = {entry["target"] for entry in manifest["papers"]}
    locked_papers = {entry["target"] for entry in lock["papers"]}
    if manifest_papers != locked_papers:
        fail(errors, "paper targets differ between manifest and lock")

    manifest_repositories = {
        (entry["target"], entry["revision"]) for entry in manifest["repositories"]
    }
    locked_repositories = {
        (entry["target"], entry["revision"]) for entry in lock["repositories"]
    }
    if manifest_repositories != locked_repositories:
        fail(errors, "repository revisions differ between manifest and lock")

    for entry in lock["papers"]:
        path = DOWNLOAD_ROOT / entry["target"]
        if not path.exists():
            continue
        if path.stat().st_size != entry["bytes"]:
            fail(errors, f"paper size mismatch: {entry['target']}")
        if sha256(path) != entry["sha256"]:
            fail(errors, f"paper hash mismatch: {entry['target']}")

    for entry in lock["repositories"]:
        path = DOWNLOAD_ROOT / entry["target"]
        if not path.exists():
            continue
        try:
            revision = git_output("rev-parse", "HEAD", cwd=path)
        except (OSError, subprocess.CalledProcessError) as error:
            fail(errors, f"cannot read repository revision {entry['target']}: {error}")
            continue
        if revision != entry["revision"]:
            fail(errors, f"repository revision mismatch: {entry['target']}")


def validate_m0(errors: list[str], require_binaries: bool) -> None:
    godot_cpp_root = ROOT / "thirdparty" / "godot-cpp"
    if not (godot_cpp_root / ".git").exists():
        fail(errors, "godot-cpp submodule is not initialized")
    else:
        try:
            revision = git_output("rev-parse", "HEAD", cwd=godot_cpp_root)
        except (OSError, subprocess.CalledProcessError) as error:
            fail(errors, f"cannot read godot-cpp revision: {error}")
        else:
            if revision != GODOT_CPP_REVISION:
                fail(
                    errors,
                    "godot-cpp revision mismatch: "
                    f"expected {GODOT_CPP_REVISION}, got {revision}",
                )

    transvoxel_cpp = (
        ADDON_ROOT / "thirdparty" / "transvoxel_mit" / "Transvoxel.cpp"
    )
    if transvoxel_cpp.is_file():
        actual_hash = sha256(transvoxel_cpp)
        if actual_hash != TRANSVOXEL_CPP_SHA256:
            fail(
                errors,
                "official Transvoxel.cpp byte hash mismatch: "
                f"expected {TRANSVOXEL_CPP_SHA256}, got {actual_hash}",
            )

    gdextension = ADDON_ROOT / "world_transvoxel.gdextension"
    if gdextension.is_file():
        descriptor = gdextension.read_text(encoding="utf-8")
        for binary in M0_BINARIES:
            resource_path = "res://" + binary.replace("\\", "/")
            if resource_path not in descriptor:
                fail(errors, f"GDExtension descriptor does not map {resource_path}")

    if require_binaries:
        for relative_path in M0_BINARIES:
            path = ROOT / relative_path
            if not path.is_file() or path.stat().st_size == 0:
                fail(errors, f"missing required M0 binary: {relative_path}")
                continue
            try:
                timestamp = pe_timestamp(path)
            except (OSError, ValueError) as error:
                fail(errors, f"invalid M0 PE binary {relative_path}: {error}")
            else:
                if timestamp != 0:
                    fail(
                        errors,
                        f"M0 PE timestamp is not normalized: {relative_path}",
                    )


def validate_license_boundaries(errors: list[str]) -> None:
    try:
        tracked = git_output("ls-files").splitlines()
    except (OSError, subprocess.CalledProcessError) as error:
        fail(errors, f"cannot inspect tracked files: {error}")
        return

    for relative_path in tracked:
        normalized = relative_path.replace("\\", "/")
        if normalized.startswith("references/downloaded/"):
            fail(errors, f"downloaded reference is tracked: {normalized}")

    for path in ROOT.rglob("Transvoxel.cpp"):
        if ".git" in path.parts or "downloaded" in path.parts:
            continue
        relative_path = path.relative_to(ROOT).as_posix()
        allowed_prefix = "addons/world_transvoxel/thirdparty/transvoxel_mit/"
        if not relative_path.startswith(allowed_prefix):
            fail(errors, f"MIT Transvoxel source is outside its boundary: {relative_path}")


def validate_source_sizes(errors: list[str], warnings: list[str]) -> None:
    addon_root = ROOT / "addons" / "world_transvoxel"
    if not addon_root.exists():
        return

    for path in addon_root.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_LIMITS:
            continue
        relative_parts = path.relative_to(addon_root).parts
        if "thirdparty" in relative_parts or "generated" in relative_parts:
            continue
        soft_limit, hard_limit = SOURCE_LIMITS[path.suffix]
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            line_count = sum(1 for _ in handle)
        relative_path = path.relative_to(ROOT).as_posix()
        if line_count > hard_limit:
            fail(
                errors,
                f"source hard limit exceeded: {relative_path} "
                f"({line_count} > {hard_limit})",
            )
        elif line_count > soft_limit:
            warnings.append(
                f"source responsibility review required: {relative_path} "
                f"({line_count} > {soft_limit})"
            )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--require-binaries",
        action="store_true",
        help="Require both M0 Windows x86-64 debug and release binaries.",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    errors: list[str] = []
    warnings: list[str] = []

    validate_required_files(errors)
    if not errors:
        validate_charter(errors)
        validate_reference_sets(errors)
        validate_m0(errors, arguments.require_binaries)
    validate_license_boundaries(errors)
    validate_source_sizes(errors, warnings)

    for warning in warnings:
        print(f"WARNING: {warning}")
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)

    if errors:
        print(f"Repository validation failed with {len(errors)} error(s).")
        return 1

    print(
        "Repository validation passed "
        f"({len(REQUIRED_FILES)} required files, {len(warnings)} warning(s))."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
