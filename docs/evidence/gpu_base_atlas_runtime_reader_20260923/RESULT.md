# GPU base-atlas runtime admission — 2026-09-23

The native `wt_open_base_coverage_atlas` reader admits only a complete,
versioned atlas for the caller's exact procedural source and LOD inventory.
Each root must carry a valid digest and either finite, indexed triangle
geometry or an explicit empty certificate. The reader rejects a non-pristine
world revision so journal replay cannot accidentally display stale base
geometry. Failed admission clears the output view; no missing root can be
inferred empty. Input bytes remain caller-owned and must live through upload.

The g23 probe file with 512 roots, 246 empty roots, 86,049 vertices, and
460,344 indices passed the new native reader in debug and release. The same
test rejected changed source identity, an edited world revision, an incomplete
inventory, a changed payload byte, a truncated payload, and an invalid empty
flag. The independent Python verifier passed the file earlier.

This is an admission boundary, not render qualification. The GPU base has not
yet been uploaded, activated, or atomically handed off to fine LOD/edit
cohorts. The 64-entry dynamic resident pool cannot hold the 266 nonempty
base roots; base coverage needs its own bounded allocation. The GPU candidate
remains unqualified for seamless movement, collision, latency, or low power.
