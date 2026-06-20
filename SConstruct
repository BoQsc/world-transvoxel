#!/usr/bin/env python

import os
import subprocess
import sys


REQUIRED_ZIG_VERSION = "0.16.0"
PROJECT_ROOT = os.path.abspath(os.getcwd())
ZIG_EXE = os.path.join(
    PROJECT_ROOT,
    ".tools",
    "zig",
    "zig.exe" if sys.platform == "win32" else "zig",
)

if not os.path.isfile(ZIG_EXE):
    raise RuntimeError(
        "Pinned Zig is missing. Run python scripts/bootstrap_toolchain.py first."
    )

zig_version = subprocess.check_output([ZIG_EXE, "version"], text=True).strip()
if zig_version != REQUIRED_ZIG_VERSION:
    raise RuntimeError(
        f"Expected Zig {REQUIRED_ZIG_VERSION}, found {zig_version} at {ZIG_EXE}"
    )

env = SConscript("thirdparty/godot-cpp/SConstruct")

zig_command = f'"{ZIG_EXE}"'
env["CC"] = zig_command + " cc"
env["CXX"] = zig_command + " c++"
env["LINK"] = zig_command + " c++"
env["SHLINK"] = zig_command + " c++"
env["AR"] = zig_command + " ar"
env["RANLIB"] = zig_command + " ranlib"

if sys.platform == "win32":
    env["ARCOM"] = "$AR $ARFLAGS $TARGET ${TEMPFILE('$SOURCES')}"

zig_cache = os.path.join(PROJECT_ROOT, ".tools", "zig-cache")
os.makedirs(zig_cache, exist_ok=True)
env["ENV"]["ZIG_GLOBAL_CACHE_DIR"] = zig_cache
env["ENV"]["ZIG_LOCAL_CACHE_DIR"] = zig_cache

env.Append(
    CPPPATH=["addons/world_transvoxel/src"],
    CXXFLAGS=["-std=c++17", "-Wall", "-Wextra"],
)

sources = (
    Glob("addons/world_transvoxel/src/*.cpp")
    + Glob("addons/world_transvoxel/src/api/*.cpp")
    + Glob("addons/world_transvoxel/src/backend/*.cpp")
    + Glob("addons/world_transvoxel/src/core/*.cpp")
    + Glob("addons/world_transvoxel/src/meshing/*.cpp")
    + Glob("addons/world_transvoxel/src/physics/*.cpp")
    + Glob("addons/world_transvoxel/src/render/*.cpp")
    + Glob("addons/world_transvoxel/src/services/*.cpp")
    + Glob("addons/world_transvoxel/src/streaming/*.cpp")
)

variant_root = os.path.join(
    "build",
    "world_transvoxel",
    "{}.{}.{}".format(env["platform"], env["target"], env["arch"]),
)
source_root = os.path.join("addons", "world_transvoxel", "src")
objects = []
for source in sources:
    relative_source = os.path.relpath(str(source), source_root)
    relative_object = os.path.splitext(relative_source)[0]
    object_target = os.path.join(variant_root, relative_object)
    objects += env.SharedObject(target=object_target, source=source)

library = env.SharedLibrary(
    "addons/world_transvoxel/bin/world_transvoxel{}{}".format(
        env["suffix"], env["SHLIBSUFFIX"]
    ),
    source=objects,
)

native_test_env = env.Clone()
native_test_sources = [
    "tests/native/test_wt_m1_cell_backend.cpp",
    "addons/world_transvoxel/src/backend/wt_cell_types.cpp",
    "addons/world_transvoxel/src/backend/wt_transvoxel_mit_backend.cpp",
]
native_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m1_cell_backend.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=native_test_sources,
)

m2_core_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m2_core.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m2_core.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/streaming/wt_lod_map.cpp",
        "addons/world_transvoxel/src/streaming/wt_stream_scheduler.cpp",
    ],
)

m2_mesh_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m2_chunk_mesh.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m2_chunk_mesh.cpp",
        "tests/native/wt_m2_mesh_test_support.cpp",
        "addons/world_transvoxel/src/backend/wt_cell_types.cpp",
        "addons/world_transvoxel/src/backend/wt_transvoxel_mit_backend.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/meshing/wt_chunk_mesh_geometry.cpp",
        "addons/world_transvoxel/src/meshing/wt_chunk_mesher.cpp",
    ],
)

m3_application_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m3_application.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m3_application.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/physics/wt_collision_apply_queue.cpp",
        "addons/world_transvoxel/src/physics/wt_collision_builder.cpp",
        "addons/world_transvoxel/src/render/wt_render_apply_queue.cpp",
        "addons/world_transvoxel/src/render/wt_render_payload.cpp",
        "addons/world_transvoxel/src/services/wt_chunk_application.cpp",
    ],
)

normalizer = os.path.join(PROJECT_ROOT, "tools", "normalize_pe_timestamp.py")


def normalize_pe_timestamp(target, source, env):
    del source
    del env
    subprocess.check_call([sys.executable, normalizer, str(target[0])])
    return 0


if env["platform"] == "windows":
    env.AddPostAction(
        library,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        native_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m2_core_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m2_mesh_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m3_application_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

Default([library, native_test, m2_core_test, m2_mesh_test, m3_application_test])
