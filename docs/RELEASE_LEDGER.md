# Release ledger

Status date: 2026-08-11.

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

Status: accepted canonical implementation; subsequent stabilization remains
classified separately from roadmap Release 4.

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
  `4d3cbadcef8d851df1061e6845444ba9ad4c02ac9671b072c92b1b944a4e2314`;
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
  pipeline;
- transition-mask updates that arrive before the target mesh has entered the
  resource cache queue the same fallback instead of failing the backend. This
  fixes the manual dig/construct regression where edit submission was rejected
  because the read-only runtime had stopped after a non-fatal transition-mask
  update race;
- edit replacement cancellation now treats stale page-meshing generations as
  benign, matching desired-set cancellation. This prevents an accepted player
  dig/place edit from stopping the runtime when viewer movement or streaming has
  already advanced the page-meshing generation.

Acceptance:

- the candidate progressed through the g23 human test and was accepted before
  Release 4 work began;
- the retained legacy pending transition-remesh path is still explicit debt
  and must be removed only with equivalent strict transition coverage.

Known excluded issue:

- the pixel-sized pinhole artifacts seen during cave inspection are
  long-standing and are explicitly deferred to a later diagnostics/test pass.
  They must not be used to block Release 3 unless this candidate clearly makes
  them worse.

### Release 4 - Parallel meshing and cancellation

Status: not implemented. The rejected Release 4 experiment is absent from
`main`; work must restart from the accepted Release 3 architecture after the
trustworthy baseline below is accepted.

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

## Trustworthy pre-Release-4 baseline candidate

Status: automated candidate passed on 2026-07-29; pending the final human
acceptance route.

This stabilization work is not roadmap Release 4. In particular, it does not
introduce the rejected parallel meshing/cancellation architecture. It repairs
and measures the accepted Release 3 runtime before any new concurrency design.

The controlled three-run Release 3 comparator established the old baseline:

- blocked movement frames: `5, 25, 29`;
- maximum consecutive blocked frames: `5, 12, 10`;
- post-flight physics-target wait: `0, 15, 38` frames;
- exact visual/collision resource readiness was not paired: visual
  `8, 7, 7`, collision `0, 1, 1`, divergence `8, 6, 6` frames.

The candidate makes the following invariants explicit:

- collision drain and application share a millisecond deadline, superseded
  collision work is coalesced, and backlog/deadline telemetry is published;
- player collision prediction follows requested movement intent and retains
  blocked intent until a true stop;
- storage requests coalesce by immutable page identity, bounded procedural
  generation workers fan one result out to all current generation waiters, and
  runtime metric snapshots are mutex-published;
- interactive edits take scheduling precedence over new viewer work;
- render and collision readiness each carry their exact resource generation;
- a replacement publishes only when every required resource matches the
  replacement generation, while an edit-local paired replacement may publish
  without waiting for unrelated travel staging;
- the chunk containing the edit-command center is scheduled before adjacent
  affected chunks;
- application-record mutation and snapshot reads are synchronized across the
  lifecycle and Godot frame threads;
- lightweight lifecycle controls bypass a render/collision payload held by a
  zero application budget, preventing publication head-of-line blocking;
- readiness repair is idempotent per chunk generation, preventing the prior
  unbounded duplicate render/collision application loop while budgets are held.

The final strict three-run integration gate, with no relaxed thresholds,
reported:

- blocked movement frames: `4, 11, 8`;
- maximum consecutive blocked frames: `2, 5, 4`;
- post-flight physics-target wait: `0, 0, 0` frames;
- edit authority commit: `2, 5, 2` frames;
- exact visual and collision readiness: `7, 8, 7` frames for both;
- visual/collision divergence: `0, 0, 0` frames;
- overall frame p95: `18.544, 17.034, 18.354` ms;
- overall frame p99: `21.759, 18.049, 20.143` ms;
- collision application remained inside the declared
  deadline-plus-one-indivisible-shape bound.

The gate artifact is
`.godot/captures/p0_candidate_foreground_ordering_3run.json` in the integration
fixture's ignored evidence directory.

The final publication/readiness qualification added on 2026-07-29 reported:

- the canonical native LOD hash remained
  `4d3cbadcef8d851df1061e6845444ba9ad4c02ac9671b072c92b1b944a4e2314`
  for 20 consecutive debug and 10 consecutive release runs;
- the zero-budget LOD fixture passed five consecutive debug and five
  consecutive release runs on each of Godot 4.6.3 and 4.7;
- the complete production Godot matrix passed for debug/release on both
  engines;
- readiness-repair submissions remained below the fixture's hard bound of 128
  render and 128 collision payloads, with zero sink failures and zero queue
  rejections;
- the sequential example route is now waited in real elapsed time and locks the
  canonical hysteresis result: 24 returned-mid leaves and 12 non-empty
  resources. The old nine-resource assertion observed an intermediate state,
  not settled terrain.

After the publication, application-record synchronization, and idempotent
repair fixes were all present in the exact integration build, the final strict
three-run P0 rerun reported:

- blocked movement frames: `14, 7, 13`;
- maximum consecutive blocked frames: `7, 2, 8`;
- post-flight physics-target wait: `0, 0, 0` frames;
- edit authority commit: `2, 1, 2` frames;
- exact visual and collision readiness: `9, 10, 8` frames for both;
- visual/collision divergence: `0, 0, 0` frames;
- overall frame p95: `16.677, 17.521, 17.634` ms;
- overall frame p99: `17.049, 18.669, 18.538` ms;
- collision application remained inside the declared
  deadline-plus-one-indivisible-shape bound in all three runs.

The final gate artifact is
`.godot/captures/p0_candidate_publication_readiness_3run.json` in the
integration fixture's ignored evidence directory.

Known exclusions remain visible rather than being silently accepted:

- the long-standing pixel-sized cave pinholes are deferred to a dedicated
  topology diagnostics and oracle pass after the roadmap releases, as directed
  during human testing;
- the non-deterministic distant-surface gap observed by the streaming gap probe
  remains a tracked baseline defect. It is not evidence of a canonical
  Transvoxel acceptance and must receive an isolated deterministic fixture.

## Cross-LOD edit publication correction

Status: authority regression passed on 2026-08-11; downstream human acceptance
remains required.

A player report exposed unchanged coarse terrain remaining visible over an
edited fine region during concurrent travel staging. The lookup tables,
density edit, and generated fine mesh were not the source. The frame-thread
publication policy allowed an edit-local fine replacement to publish while an
overlapping coarse chunk was still retained by the LOD transition.

The corrected policy promotes that replacement to regional publication. The
old render and old collision remain paired until the replacement region can
swap. A controlled mutation reproduced five invalid visible overlap frames
between coarse chunk (1,0,0,L1) and fine chunk (2,0,0,L0); the corrected Godot
4.7 debug and release tests produced zero mixed-ownership frames.

## Rule for continuing

Release 2 and Release 3 have been accepted for progression with their explicit
debt tracked above. Do not restart Release 4 until the trustworthy pre-Release-4
baseline candidate passes its final human route. Parallel meshing, page
acceleration, and render cleanup stay blocked until their predecessor release
gates pass.
