# Offline GPU base-atlas probe — 2026-09-23

The standalone native probe bakes the complete g23 LOD3 root inventory from
the same procedural descriptor as the game (seed 19023, source revision
190327, 128×16×128 chunks, origin Y -8, bedrock boundary). It uses the
official MIT Transvoxel mesher. This first format stores regular root geometry
at one uniform LOD, so no transition faces are needed in the base cut. Each
record has an explicit empty flag and SHA-256 digest. The file header carries
version, source identity, world dimensions, boundary policy, LOD, and count.

One full debug run produced 512 roots, 246 proven empty records, 86,049
vertices, 460,344 indices (153,448 triangles), and a 4,195,475-byte file in
41,905 ms. A second full run with page-backed parity enabled took 47,463 ms.
For every root, the direct-source mesh bytes matched the mesh made from a
generated, decoded runtime page. Both complete files had SHA-256
`c61a6905b95030a1b73e409c49732a27c32951db87c4f5c7bd1089fb75b9fe70`.
An eight-root release parity run matched its debug output byte for byte.
The verifier checked every record digest, key, finite vertex, index range,
explicit empty flag, and exact file boundary. A flipped payload byte was
rejected. Debug and release native builds passed.

Reproduce with `build/tools/wt_base_coverage_atlas_probe.template_debug.x86_64.exe
build/atlas_probe_512.wtba 512 --page-parity` and
`python tools/verify_base_coverage_atlas_probe.py build/atlas_probe_512.wtba`.
The generated atlas remains in ignored `build/` output, not the release addon.

This measures offline geometry and validates parity only. There is no runtime
atlas reader, GPU upload, base-cut activation, journal invalidation, or power
measurement yet. The GPU candidate remains unqualified.
