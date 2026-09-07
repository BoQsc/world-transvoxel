# GPU stale storage cancellation checkpoint

Superseded and released page-meshing generations now remove immutable source
page requests that are still queued and have no surviving meshing owner. Work
already in flight completes normally. Shared meshing dependencies and requests
also owned by interaction warming remain intact.

The storage service records request source and exposes total and interaction
queued-cancellation metrics. The page runtime exposes the number of dependency
requests it reclaimed. This preserves the configured capacities and does not
change terrain authority, persistence, topology, collision, or gameplay APIs.

Debug and release native checks passed:

- `test_wt_m5_async_storage`
- `test_wt_m5_page_meshing_runtime`
- `test_wt_m5_workload`
- `test_wt_production_streaming`
- `test_wt_production_lod_streaming`

The D3D12 trace-on large relocation route completed with one blocked movement
frame, compared with 196 in the preceding checkpoint. Its relocated edit
storage queue segments were measured in milliseconds rather than seconds.
The Vulkan trace-on route completed but recorded 124 blocked movement frames;
its retained critical paths identify scheduler queue residency and visual
cohort staging as the remaining dominant dependencies. This checkpoint is an
isolated storage-lane correction, not GPU-path promotion.
