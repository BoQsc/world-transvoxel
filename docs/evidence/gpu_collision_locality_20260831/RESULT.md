# Explicit Collision Demand During Visual Retirement

Status: `FOCUSED_NATIVE_REGRESSION_PASSED`

## Defect and Scope

The outgoing visual-retirement path unconditionally promoted visual-only chunks
to collision-required when it could rebuild regular collision from cached mesh
geometry. In GPU mode this could resurrect CPU triangles for a chunk that had
previously supported the player but whose local collision demand was withdrawn.

With `visual_viewer_collision_enabled=false`, explicit collision viewers now
remain authoritative during visual retirement. The unnecessary promotion and
rebuild are skipped. Already-required colliders still use the existing
front-end coverage/retirement mechanism. The default broad visual-collision
mode is unchanged. No visual selection, transition-mask checks, publication
cohort, collision readiness guard, shader, or readback policy changed.

## Reproducer

`test_wt_production_streaming --collision-locality` uses a four-page baked plane.
It establishes local physical support, moves physical demand to another page,
then moves the visual viewer after the old support page has become visual-only.
The old page retains real CPU mesh data even when GPU visuals use placeholders.

The regression covers zero and one mesh workers in each of three configurations:
explicit collision with CPU visuals, explicit collision with GPU visuals, and
the legacy broad visual-collision control.

| Configuration | Before: outgoing collision triangles | After | New support triangles |
| --- | ---: | ---: | ---: |
| CPU visuals, explicit collision | 1,536 | 0 | 512 |
| GPU visuals, explicit collision | 512 | 0 | 512 |
| Legacy broad visual collision | 1,024 | 1,024 | 512 |

Both worker configurations produce those counts. Log `faces` counts triangle
vertices, so divide by three to obtain the triangle counts above. Before the
fix, all four explicit-demand cases failed their no-repromotion assertion;
the two legacy controls passed. After the fix, all six pass in both debug and
release builds using `build_profile=build_profiles/world_transvoxel.json`.

The tests also require visual completion, no loss of the new support demand,
and zero CPU visual vertices/indices in GPU mode. The complete production
streaming executable, publication-policy oracle, and M3 collision/application
regressions pass in release configuration. Downstream engine checks and timing
results are recorded in the integration game's checkpoint, not inferred here.

## Claim Boundary

This proves removal of unnecessary collision work in the isolated lifecycle.
It does not prove smooth gameplay, a CPU/GPU speedup, or completion of GPU
terrain. The separate large publication-cohort delay remains open. CPU physics
still consumes triangles for demanded local collision; GPU visuals do not.
