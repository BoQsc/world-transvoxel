# Release ledger

Status date: 2026-07-26.

This ledger separates the ordered roadmap releases from stabilization commits
made during human testing. Commit messages alone are not sufficient release
evidence. A roadmap release is complete only when its listed exit evidence
exists after the relevant code changes.

## Roadmap release order

### Release 1 - Coordinate correctness

Status: evidence audit required before closure.

Scope:

- retain authoritative integer world coordinates;
- render chunk-local or camera-relative vertices;
- add a large-coordinate Godot integration fixture;
- reconcile the M3 render/collision contract and status documentation.

Human result:

- the same terrain can be visited at deliberately extreme coordinates without
  collapsing, vibrating, or cracking.

Exit evidence:

- ordinary-world images remain equivalent;
- large-coordinate fixture passes;
- human route passes once.

### Release 2 - Frame-stable collisions

Status: candidate accepted for progression; not complete against the strict
frame-time gate.

Scope:

- replace item-count collision bursts with millisecond/deadline budgeting;
- coalesce superseded collision work;
- introduce collision-specific LOD or decimation if profiling proves it is
  needed;
- prevent collision shape application from consuming an unrestricted frame;
- expose collision backlog and collision application timing in diagnostics.

Human result:

- movement and edits no longer produce the documented 30-135 ms collision
  spikes.

Exit evidence on the reference machine:

- overall frame p95 is at most 16.67 ms for the 60 FPS target;
- overall frame p99 is at most 33.3 ms;
- terrain collision application stays inside its assigned per-frame deadline;
- any simplified collision path is accepted physically and visually here, not
  deferred into later releases.

Candidate evidence gathered on 2026-07-25:

- native focused checks passed:
  `test_wt_m3_application` debug/release,
  `test_wt_m5_resource_cache` release,
  `test_wt_production_lod_streaming` release;
- short native soak passed with `frame_max_ns=8330400`;
- g23 production quality passed with `spawn_floor_hit=1`,
  `total_collision_backlog=0`, and
  `collision_apply_time_ns_maximum=22904200`;
- g19 production quality and `streaming_fly_gap_gate` passed with
  `spawn_floor_hit=1`, `collision_required_not_ready=0`, no visual gap failures,
  and `collision_apply_time_ns_maximum=26594700`.

Human acceptance:

- the Release 2 candidate was accepted for progression after human testing on
  2026-07-25/2026-07-26. This means Release 3 may proceed; it does not erase
  the remaining Godot collision publication debt below.

Remaining blocker:

- Godot `ConcavePolygonShape3D::set_faces` / collision shape publication can
  still consume more than the assigned 4 ms collision deadline for a single
  chunk. Triangle-dropping caps below 512 were rejected because they created
  physical collision holes, and split multi-shape publication was rejected
  because it increased Godot-side spikes. The next Release 2 step must use a
  physically coherent collision LOD/shape representation or a lower-level
  physics publication path; do not reintroduce triangle deletion as the default.

### Release 3 - Canonical mesh and transition representation

Status: candidate implemented; source gates pass; pending human LOD-boundary
flight acceptance.

Scope:

- establish strict canonical and production-robust profiles;
- add an independent full-pipeline oracle;
- use official vertex-reuse metadata;
- store regular primary and secondary boundary positions;
- cache six reusable transition faces;
- ensure a transition-mask-only change performs zero density sampling and zero
  remeshing.

Human result:

- repeated flight across LOD boundaries shows no new seam, winding, transition,
  or streaming-stutter regression.

Exit evidence:

- official oracle passes;
- no new seam or winding failures;
- mask-only updates do not enqueue mesh jobs;
- the old mask-baked path is removed after acceptance instead of maintained as
  a second production path.

Candidate evidence gathered on 2026-07-26:

- release build passed:
  `scons platform=windows target=template_release arch=x86_64 -j4`;
- focused Release 3 native gates passed:
  `test_wt_m2_chunk_mesh`,
  `test_wt_m5_page_transition`,
  `test_wt_m5_page_meshing_runtime`,
  `test_wt_m5_resource_cache`, and
  `test_wt_production_lod_streaming`;
- canonical M2 mesh hash:
  `f3ebfec883e2de19`;
- page transition hash:
  `1761e09383752d56`;
- page meshing runtime hash:
  `ea428b532f85d6b1abb751856e27532c49acff9f2d320a5756a1723165908ad7`;
- resource cache hash:
  `ecf7cbc1ddb527fbcbde760e7c6bd7b016f44c10df1bc7fb51540c9012b1f7c3`;
- production LOD streaming hash:
  `c5d0fcada4007b6d13fa5665174abdc89e9923dced40e6c2f60e544c1e6ccc5a`;
- production LOD evidence showed the bridge chunk retained generation while
  the render transition mask changed (`bridge0=320/1674`,
  `bridge1=320/1674`), no transition remesh generations were staged, and
  transition completions remained bounded at `2`.

Startup regression fix on 2026-07-26:

- the first Release 3 candidate requested six cached transition faces for every
  non-LOD0 chunk. The g23 human startup gate stayed behind `Loading terrain...`
  with hundreds of extra sampling records because that policy exploded page
  dependencies for chunks that had no active transition face;
- the cache request policy was narrowed to request the reusable six-face
  transition cache only for chunks with an active transition mask. Existing
  zero-mask chunks that later require a transition face use the retained legacy
  remesh fallback until the Release 3 human candidate is accepted and the final
  strict cache policy can be locked;
- mesh completions whose current plan mask is no longer supported by the
  completed mesh publish the mesh's own safe transition mask and queue that
  same legacy transition remesh fallback, rather than failing the runtime
  pipeline.

Remaining before acceptance:

- run the integration-game human test and fly repeatedly across LOD
  boundaries on the g23 profile;
- accept or reject the candidate based on seam, winding, transition, movement,
  and streaming-stutter behavior;
- after acceptance, remove the retained legacy pending transition-remesh path
  instead of keeping two production architectures.

Known excluded issue:

- the pixel-sized pinhole artifacts seen during cave inspection are
  long-standing and are explicitly deferred to a later diagnostics/test pass.
  They must not be used to block Release 3 unless this candidate clearly makes
  them worse.

### Release 4 - Parallel meshing and cancellation

Status: blocked until Release 3 is complete.

Scope:

- bounded CPU-dependent worker count;
- immutable jobs and generation tokens;
- reusable scratch per worker;
- cooperative cancellation checkpoints;
- bounded completion/publication queues;
- priority inversion and starvation tests.

Exit evidence:

- output hashes remain identical to the canonical single-thread path;
- obsolete jobs are cancelled before publication;
- throughput improves measurably on the reference machine;
- frame spikes and memory remain within the agreed ceiling.

### Release 5 - Empty-space and page acceleration

Status: blocked until Release 4 is complete.

Scope:

- page density min/max;
- uniform density/material metadata;
- surface-present flag;
- water-present flag;
- direct transition-support lookup;
- early rejection before constructing meshing state.

Exit evidence:

- uniform air/solid pages create no mesh job;
- water-absent pages require no water sample scan or second mesh;
- visible output is identical;
- CPU, I/O, and memory are reduced on the locked route.

### Release 6 - Profile-guided rendering cleanup

Status: conditional.

This release exists only if profiling after Release 2 still identifies GPU time,
render submission, draw-call count, or render memory as a bottleneck.

Potential scope:

- bake road/biome classification;
- skip zero-weight authored paths;
- remove unused UV2/color/normal data;
- compact material weights;
- group render meshes;
- consider direct `RenderingServer` RIDs.

Exit evidence:

- every accepted change shows a GPU-time, draw-call, memory, or frame-time
  improvement. If the GPU already meets its budget, close this audit without
  code changes.

## Stabilization commits are not roadmap releases

The following pushed commits were necessary stabilization work discovered during
human testing. They must not be counted as completion of roadmap Release 2 or
Release 3.

| Commit | Label used at the time | Correct classification |
| --- | --- | --- |
| `c6aa2a0` | `Release 2: stabilize staged edit replacement streaming` | Stabilization A: staged edit/replacement streaming safety |
| `5dfa9fa` | `Release 3: repair visual readiness after staged swaps` | Stabilization B: visual readiness after staged swaps |

## Rule for continuing

Release 2 has been accepted for progression with collision publication debt
tracked above. Do not begin Release 4 until the Release 3 human LOD-boundary
test accepts the canonical transition candidate. Parallel meshing, page
acceleration, and render cleanup stay blocked until their predecessor release
gates pass.
