# Same-callback loaded-edit activation checkpoint

Authority base: `576c8cd1d3202bc35e4b3e5eec5271de29b752e5`

The native activation-cohort query now exposes the immutable cohort shape as
soon as its publication graph is built. This lets the integration controller
identify an exact one-chunk, same-layout incremental replacement before any
readiness query mutates request state. Regional, multi-chunk, transition-mask,
retirement, LOD, and streaming cohorts retain the existing deferred path.

After normal native preparation and request validation, the eligible hot-edit
cohort is precommitted and its GPU activation waits behind extraction in the
same render callback. GPU cohort validation remains atomic and retains old
coverage on any missing, stale, or failed candidate.

| Driver | Submission maximum | First draw maximum | Observed-ready maximum |
| --- | ---: | ---: | ---: |
| Vulkan | 463 us | 25.050 ms | 56.070 ms |
| D3D12 | 464 us | 28.269 ms | 57.099 ms |

All six traced edits on each driver recorded `NATIVE_PREPARED`,
`COHORT_SELECTED`, and `ACTIVATION_REQUESTED` with
`same_callback_precommit=true`. The render-thread `FIRST_DRAW` timestamp meets
the 33.334 ms two-displayed-frame gate; the later observed-ready value includes
test-coroutine observation delay.

Collision continuity retained support for 600 frames on both drivers. The
two-chunk rapid-edit route retained zero mixed revisions, 24 incremental
dispatches, zero copy fallbacks, and 43 completed retirements on both drivers.
Discarded asynchronous summaries count as completions without transferred
bytes, so the route now checks the exact 20-byte contract against successful
readback requests rather than all completions.

Seven native regressions passed in both debug and release. Production streaming
retained hash
`39db05c67fc2f4b8d8beaab2e7da927ae968efb3d75118bcd80c5523116d9b3b`,
and production LOD streaming retained hash
`1a59569e2131a7aa07279004a8c2ce304278da658047c3aac8d56bb601ae87a3`.
The executable digests and complete outputs are retained in
`native_tests.json`; traced driver timelines are retained beside this result.
