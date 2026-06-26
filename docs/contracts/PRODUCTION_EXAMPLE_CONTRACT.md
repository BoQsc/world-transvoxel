# Production Example Contract

Status: complete; closes PQ1

## Scope

The root `world_transvoxel/` directory is the application side of the addon
boundary. It contains:

- `example_world.tscn`, a real terrain/viewer/camera/light scene;
- `example_config.tres`, bounded schema-1 runtime settings;
- `example_world.gd`, lifecycle and viewer-event scaffolding;
- `example_viewer.gd`, transform-notification to viewer-event adaptation;
- a concise usage README.

It does not parse pages, balance LODs, construct meshes, schedule jobs, build
collision, or own worker state. Those responsibilities remain inside
`addons/world_transvoxel/` and its native tools.

## World preparation

`scripts/prepare_example_world.py` writes a deterministic dense plane source
under ignored `build/`, then calls the public `tools/wt_bake.py` entry point.
The native release baker produces 28 content-addressed LOD0/LOD1 pages and a
validated `world.wtworld` manifest. No mesh or topology fixture is stored in
GDScript or committed as binary example data.

The debug/release bake identity is locked to:

```text
e44d8955cbe64ce8d606d3a906ba68805014b02ec16edc38f58dad7c6062336a
```

The example is the repository development/application scene. PQ3 separately
proves a generated clean project containing the copied distributable addon,
required notices, deterministic world fixture, and test-only scaffolding.

## Runtime behavior

The selected root main scene starts the manifest asynchronously. Once running,
the viewer adapter submits its initial position. Later `Node3D` transform
notifications submit monotonic viewer revisions without a per-frame polling
loop. The native production coordinator owns the global 2:1 plan, transition
masks, page I/O, official MIT meshing, bounded Godot resource application, and
shutdown.

## Proof

`production_example_test.gd` loads the selected main scene and proves:

- the 28-page hierarchical manifest starts through the public facade;
- the initial fine/coarse render and collision resources settle;
- two transform events produce the expected six- and nine-surface balanced
  plans;
- real LOD0 and LOD1 node identities are present;
- explicit shutdown reaches `stopped` with zero retained resources.

The test passes with debug and release addon binaries on Godot 4.6.3 and 4.7.
The production qualification entry point runs it before the complete M5-to-M0
regression chain.

## PQ1 exit

PQ1 is complete. A thin real scene now bakes, loads, streams, renders,
collides, moves, and shuts down against the official MIT-backed read-only
runtime. Public editing, query, and persistence integration begin in PQ2.
