# World Transvoxel Example

This directory is the thin application side of the addon boundary. The scene
configures `WorldTransvoxelTerrain`, converts transform notifications into
viewer events, and starts/stops a baked world. It contains no page parsing,
LOD balancing, meshing, scheduling, or collision generation.

From the repository root:

```console
python scripts/build.py --configuration all
python scripts/prepare_example_world.py
```

Then open or run `world_transvoxel/example_world.tscn` in Godot. The example
loads `build/world-transvoxel-example/world.wtworld`, which contains 28 real
LOD0/LOD1 pages baked from a plane through the public native bake tool.

Move the `Viewer` node to issue an event-driven viewer update. The addon owns
the global 2:1 plan, asynchronous page loading, official MIT transition
meshing, bounded render/collision application, and shutdown.

The root project is the development/example project. Copying only the
distributable addon into a separate clean project is intentionally a later PQ3
qualification gate.
