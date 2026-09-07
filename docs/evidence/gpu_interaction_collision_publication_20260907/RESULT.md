# Interaction collision publication checkpoint

Authority base: `d29c0b4eb75e8d0cee86b086b8c31a66ae6bb917`

Player-support and interaction-focus chunks now carry interaction urgency from
native completion through the bounded publication and application queues. A
critical collision may update the authoritative application record and Godot
physics sink while its staged GPU visual is still pending. Same-key expectation
and positive collision-requirement messages remain ordering prerequisites.

The application regression proves that an interaction-critical generation-four
collision applies while its staged visual remains unready. The production
foreground regression warms an offscreen page, demands it later, verifies that
storage completion preceded demand, and verifies that its collision publication
is critical. The saturated-capture supersede regression still rejects stale
visual and collision generations.

The traced cold target `{6,0,0,0}` reached the collision sink independently of
visual readiness:

| Driver | Collision prepared to sink | Sink work | Sink before visual | Combined target ready |
| --- | ---: | ---: | ---: | ---: |
| Vulkan | 17.897 ms | 0.555 ms | 33.188 ms | 84.060 ms |
| D3D12 | 8.827 ms | 0.505 ms | 50.302 ms | 100.856 ms |

Both traces mark the collision publication critical and contain seven collision
sink applications for six hot edits plus the cold target. Submission remained
below 1 ms: 470 us maximum on Vulkan and 388 us on D3D12. The combined readiness
metric still waits for GPU LOD0 visual activation, so this checkpoint does not
claim instant visual terrain.

Seven native regressions passed in both debug and release. Production streaming
retained hash
`39db05c67fc2f4b8d8beaab2e7da927ae968efb3d75118bcd80c5523116d9b3b`.
The complete outputs and executable digests are in `native_tests.json`; the two
driver timelines are retained beside this result.
