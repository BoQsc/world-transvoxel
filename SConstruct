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
if env["platform"] == "linux":
    env.Append(CXXFLAGS=["-pthread"], LINKFLAGS=["-pthread"])

sources = (
    Glob("addons/world_transvoxel/src/*.cpp")
    + Glob("addons/world_transvoxel/src/api/*.cpp")
    + Glob("addons/world_transvoxel/src/backend/*.cpp")
    + Glob("addons/world_transvoxel/src/bake/*.cpp")
    + Glob("addons/world_transvoxel/src/core/*.cpp")
    + Glob("addons/world_transvoxel/src/editing/*.cpp")
    + Glob("addons/world_transvoxel/src/meshing/*.cpp")
    + Glob("addons/world_transvoxel/src/physics/*.cpp")
    + Glob("addons/world_transvoxel/src/render/*.cpp")
    + Glob("addons/world_transvoxel/src/services/*.cpp")
    + Glob("addons/world_transvoxel/src/storage/*.cpp")
    + Glob("addons/world_transvoxel/src/streaming/*.cpp")
    + Glob("addons/world_transvoxel/src/testing/*.cpp")
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

m4_storage_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m4_storage.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m4_storage.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
    ],
)

m4_bake_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m4_bake.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m4_bake.cpp",
        "addons/world_transvoxel/src/bake/wt_chunk_baker.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
    ],
)

m4_world_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m4_world.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m4_world.cpp",
        "addons/world_transvoxel/src/bake/wt_chunk_baker.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
        "addons/world_transvoxel/src/storage/wt_world_manifest.cpp",
    ],
)

m4_edit_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m4_edit.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m4_edit.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_transaction.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_types.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
    ],
)

m4_spatial_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m4_spatial.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m4_spatial.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_spatial_index.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_types.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
    ],
)

m4_journal_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m4_journal.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m4_journal.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_journal.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_transaction.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_types.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
    ],
)

m4_apply_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m4_apply.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m4_apply.cpp",
        "addons/world_transvoxel/src/bake/wt_chunk_baker.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/editing/wt_chunk_edit_state.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_journal.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_transaction.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_types.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
    ],
)

m4_compaction_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m4_compaction.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m4_compaction.cpp",
        "addons/world_transvoxel/src/bake/wt_chunk_baker.cpp",
        "addons/world_transvoxel/src/bake/wt_snapshot_compactor.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/editing/wt_chunk_edit_state.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_journal.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_transaction.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_types.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
        "addons/world_transvoxel/src/storage/wt_world_manifest.cpp",
    ],
)

m5_async_storage_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m5_async_storage.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m5_async_storage.cpp",
        "addons/world_transvoxel/src/bake/wt_chunk_baker.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/storage/wt_async_storage_service.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
        "addons/world_transvoxel/src/storage/wt_world_manifest.cpp",
    ],
)

m5_storage_cache_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m5_storage_cache.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m5_storage_cache.cpp",
        "addons/world_transvoxel/src/bake/wt_chunk_baker.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
        "addons/world_transvoxel/src/storage/wt_storage_page_cache.cpp",
    ],
)

m5_resource_cache_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m5_resource_cache.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m5_resource_cache.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/physics/wt_collision_builder.cpp",
        "addons/world_transvoxel/src/render/wt_render_payload.cpp",
        "addons/world_transvoxel/src/services/wt_chunk_resource_cache.cpp",
        "addons/world_transvoxel/src/services/wt_chunk_resource_payload.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
    ],
)

m5_multi_viewer_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m5_multi_viewer.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m5_multi_viewer.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
        "addons/world_transvoxel/src/streaming/wt_multi_viewer_desired_set.cpp",
    ],
)

m5_edit_replacement_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m5_edit_replacement.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m5_edit_replacement.cpp",
        "addons/world_transvoxel/src/bake/wt_chunk_baker.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_spatial_index.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_types.cpp",
        "addons/world_transvoxel/src/physics/wt_collision_apply_queue.cpp",
        "addons/world_transvoxel/src/physics/wt_collision_builder.cpp",
        "addons/world_transvoxel/src/render/wt_render_apply_queue.cpp",
        "addons/world_transvoxel/src/render/wt_render_payload.cpp",
        "addons/world_transvoxel/src/services/wt_chunk_application.cpp",
        "addons/world_transvoxel/src/services/wt_chunk_resource_cache.cpp",
        "addons/world_transvoxel/src/services/wt_chunk_resource_payload.cpp",
        "addons/world_transvoxel/src/services/wt_edit_runtime_replacement.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
        "addons/world_transvoxel/src/storage/wt_storage_page_cache.cpp",
        "addons/world_transvoxel/src/streaming/wt_stream_scheduler.cpp",
    ],
)

m5_workload_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m5_workload.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m5_workload.cpp",
        "tests/native/wt_m5_workload_fixture.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_spatial_index.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_types.cpp",
        "addons/world_transvoxel/src/physics/wt_collision_apply_queue.cpp",
        "addons/world_transvoxel/src/physics/wt_collision_builder.cpp",
        "addons/world_transvoxel/src/render/wt_render_apply_queue.cpp",
        "addons/world_transvoxel/src/render/wt_render_payload.cpp",
        "addons/world_transvoxel/src/services/wt_chunk_application.cpp",
        "addons/world_transvoxel/src/services/wt_chunk_resource_cache.cpp",
        "addons/world_transvoxel/src/services/wt_chunk_resource_payload.cpp",
        "addons/world_transvoxel/src/services/wt_desired_set_runtime.cpp",
        "addons/world_transvoxel/src/services/wt_edit_runtime_replacement.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
        "addons/world_transvoxel/src/storage/wt_storage_page_cache.cpp",
        "addons/world_transvoxel/src/streaming/wt_multi_viewer_desired_set.cpp",
        "addons/world_transvoxel/src/streaming/wt_stream_scheduler.cpp",
    ],
)

m5_pipeline_budget_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m5_pipeline_budget.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m5_pipeline_budget.cpp",
        "addons/world_transvoxel/src/backend/wt_cell_types.cpp",
        "addons/world_transvoxel/src/backend/wt_transvoxel_mit_backend.cpp",
        "addons/world_transvoxel/src/bake/wt_chunk_baker.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/meshing/wt_chunk_mesh_geometry.cpp",
        "addons/world_transvoxel/src/meshing/wt_chunk_mesher.cpp",
        "addons/world_transvoxel/src/storage/wt_async_storage_service.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page_sample_source.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
        "addons/world_transvoxel/src/storage/wt_world_manifest.cpp",
    ],
)

m5_page_transition_test = native_test_env.Program(
    os.path.join(
        "build",
        "native-tests",
        "test_wt_m5_page_transition.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tests/native/test_wt_m5_page_transition.cpp",
        "tests/native/wt_m2_mesh_test_support.cpp",
        "addons/world_transvoxel/src/backend/wt_cell_types.cpp",
        "addons/world_transvoxel/src/backend/wt_transvoxel_mit_backend.cpp",
        "addons/world_transvoxel/src/bake/wt_chunk_baker.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/meshing/wt_chunk_mesh_geometry.cpp",
        "addons/world_transvoxel/src/meshing/wt_chunk_mesher.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page_sample_source.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
    ],
)

storage_tool = native_test_env.Program(
    os.path.join(
        "build",
        "tools",
        "wt_storage_tool.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tools/native/wt_storage_tool.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_journal.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_transaction.cpp",
        "addons/world_transvoxel/src/editing/wt_edit_types.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
        "addons/world_transvoxel/src/storage/wt_world_manifest.cpp",
    ],
)

bake_tool = native_test_env.Program(
    os.path.join(
        "build",
        "tools",
        "wt_bake_tool.{}.{}{}".format(
            env["target"],
            env["arch"],
            ".exe" if env["platform"] == "windows" else "",
        ),
    ),
    source=[
        "tools/native/wt_bake_tool.cpp",
        "addons/world_transvoxel/src/bake/wt_chunk_baker.cpp",
        "addons/world_transvoxel/src/bake/wt_dense_grid_source.cpp",
        "addons/world_transvoxel/src/core/wt_chunk_key.cpp",
        "addons/world_transvoxel/src/storage/wt_binary_io.cpp",
        "addons/world_transvoxel/src/storage/wt_chunk_page.cpp",
        "addons/world_transvoxel/src/storage/wt_container_format.cpp",
        "addons/world_transvoxel/src/storage/wt_hash256.cpp",
        "addons/world_transvoxel/src/storage/wt_world_manifest.cpp",
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

    env.AddPostAction(
        m4_storage_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m4_bake_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m4_world_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m4_edit_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m4_spatial_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m4_journal_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m4_apply_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m4_compaction_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m5_async_storage_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m5_storage_cache_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m5_resource_cache_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m5_multi_viewer_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m5_edit_replacement_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m5_workload_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m5_pipeline_budget_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        m5_page_transition_test,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        storage_tool,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

    env.AddPostAction(
        bake_tool,
        Action(normalize_pe_timestamp, "Normalizing PE timestamp $TARGET ..."),
    )

Default([
    library,
    native_test,
    m2_core_test,
    m2_mesh_test,
    m3_application_test,
    m4_storage_test,
    m4_bake_test,
    m4_world_test,
    m4_edit_test,
    m4_spatial_test,
    m4_journal_test,
    m4_apply_test,
    m4_compaction_test,
    m5_async_storage_test,
    m5_storage_cache_test,
    m5_resource_cache_test,
    m5_multi_viewer_test,
    m5_edit_replacement_test,
    m5_workload_test,
    m5_pipeline_budget_test,
    m5_page_transition_test,
    storage_tool,
    bake_tool,
])
