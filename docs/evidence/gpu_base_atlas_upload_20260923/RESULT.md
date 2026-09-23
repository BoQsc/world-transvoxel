# Packaged GPU base atlas and native upload preparation — 2026-09-23

The g23 LOD3 atlas is packaged as
`addons/world_transvoxel/data/g23_base_lod3.wtba` (4,195,475 bytes,
SHA-256 `c61a6905b95030a1b73e409c49732a27c32951db87c4f5c7bd1089fb75b9fe70`).
The internal native Godot bridge admits it only for a running, pristine,
matching procedural source and returns packed shared GPU buffers: world-space
positions, octahedral normals, material metadata, indices, and 266 indexed
indirect commands. The 246 proven-empty roots still participate in the exact
512-root inventory.

The native debug and release atlas tests pass against the packaged file.
The independent Python verifier passes. A Godot 4.7.2 headless test started
the matching g23 source, requested the packaged atlas, and checked its 512
roots, 266 draws, 86,049 vertices and 460,344 indices. Upload preparation
took 48.203 ms in that single run; this is a startup measurement, not a p99.

The integration-game renderer and GPU behavior are qualified separately.
These checks prove input validation and byte packing, not visible coverage,
atomic LOD handoff, edit latency, collision behavior, or power consumption.
