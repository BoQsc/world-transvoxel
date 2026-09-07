# Interaction visual admission checkpoint

Exact interaction placeholders can now be pulled and applied before their matching incremental GPU request is admitted. Collision readiness remains authoritative; a request whose application record is still unavailable waits in a bounded controller queue without occupying a GPU capture slot.

## Focused results

- Five consecutive Vulkan cross-brick trace runs: 30/30 edits passed; submit maximum 374-756 us; first draw maximum 26.4-27.5 ms; cached approach 83.7-84.2 ms.
- Vulkan/D3D12, single/cross brick, trace on/off: all 8 configurations passed. Submit maximum 322-522 us except the retained five-run Vulkan sample above; first draw maximum 27.1-32.0 ms; cached approach 83.1-84.7 ms.
- First edit: Vulkan and D3D12 visible at frame 2; commit at frame 0.
- Rapid edit: 12 edits per backend, 12 same-layout cohorts, 24 chunks, 24 incremental dispatches, zero mixed revisions and zero copy fallbacks.
- Collision continuity: Vulkan and D3D12 each retained support for 600 frames at y=7.969 with one authoritative collision resource.
- Automatic LOD relocation: Vulkan 8/5/7 ready frames; D3D12 7/5/6.
- Autonomous large-world route: D3D12 exited cleanly; the first Vulkan run exposed teardown without controller stop. Adding controller tree-exit cleanup made the repeated full Vulkan GPU route exit 0. The route still misses production promotion gates: Vulkan recorded 10 blocked movement frames and D3D12 196, with p95 frame times 51.6 ms and 42.0 ms.

## Native regressions

The seven selected debug and release executables all exited 0: production streaming, production LOD streaming, production lifecycle, M5 edit replacement, M3 application, publication policy, and GPU meshing shadow.

This checkpoint removes the loaded first-edit frontend admission race. It does not qualify the overall GPU terrain path; cold approach and large-world movement remain subsequent checkpoints.
