# Production Edited-Terrain LOD Correctness Contract

This contract defines what World Transvoxel may claim about player-edited or
tool-edited terrain when streaming and LOD are active. It exists because three
different properties are easy to confuse:

- authoritative edit truth;
- rendered/collision LOD representation;
- visible continuity while chunks are generated, replaced, or simplified.

Passing one property does not automatically prove the other two.

## Definitions

Authoritative edit truth is the durable world state produced by committed edit
transactions, append-only journals, compacted snapshots, and authoritative
sample queries. It is not allowed to disappear because the viewer moved away,
because a runtime cache was evicted, or because the active LOD set changed.

Rendered LOD representation is the bounded mesh and collision state currently
resident for viewers. It may be simplified according to the selected profile,
active chunk capacity, viewer radii, and edit-retention limits.

Exact edited region means a configured region around active viewers, active
edit sites, or map-editor focus areas where edited terrain must be represented
at the configured exact/high-detail LOD and collision must agree with the same
authoritative samples.

Shape continuity means a player can move from close to mid to far and back
without seeing an edited hole, tunnel, or placed feature restore, disappear,
or change harshly beyond the chosen profile's accepted tolerance.

## Non-negotiable guarantees

1. Committed edit truth is durable. Runtime LOD retention, cache eviction, and
   active-set pressure must not delete or overwrite the source edit state.
2. Loaded generations that depend on an edited region must be invalidated or
   replaced from authoritative edited samples before they are treated as current.
   This includes same-LOD chunks, dependent coarser LOD support pages, and
   transition support where the profile uses them.
3. Once initial terrain for a human-visible scene is ready, old visible render
   and collision state must remain until replacement state is ready. A profile
   must not expose empty rectangular sky patches during ordinary movement or
   edit replacement.
4. Player-near edited terrain must be exact for the profile. Digging, placing,
   and returning to the edited area must not restore old terrain or hide edits
   behind a coarse representation near the player.
5. Collision and render must be derived from compatible authoritative state.
   A player must not collide with restored terrain while the visible mesh shows
   a dug tunnel, or fall through visible terrain because collision was retired
   earlier than render.
6. Mesh watertightness, edit persistence, and far-distance shape continuity are
   separate claims. A zero-open-edge mesh probe does not prove that distant
   edited holes will preserve their close-range silhouette.
7. Presentation features are not correctness proofs. Double-sided materials,
   hidden full-map surfaces, backdrop terrain, fade effects, and crossfades may
   be optional visual features, but they must not be used to hide missing
   terrain, lost edits, or nonmanifold holes.

## Allowed limits

World Transvoxel streams a bounded active chunk set. The default production
path does not promise every edited region remains full-resolution from every
future camera distance.

A project may choose to let distant or old edited regions simplify when all of
the following are true:

- authoritative edit truth is still exact and queryable;
- returning to the edited area restores the exact/high-detail representation
  before the player can interact with it;
- the simplification is documented for that profile;
- close/mid/far/return validation proves the visual result is acceptable for
  the profile's camera distances and movement speeds.

A project that needs all player or map edits to remain exact everywhere must
use an explicit exact-global-edit-visibility profile or an editor/bake workflow
that generates and validates the necessary multi-LOD edited pages. That is a
separate, higher-cost claim and is not the default streaming claim.

## Map-editor exactness

Map-authoring tools have stricter expectations than normal runtime streaming.
An editor must be able to force an exact working region, keep its edited pages
resident or synchronously regenerated, and validate/bake derived LOD data before
shipping the world. Runtime recent-edit retention heuristics are not enough for
authoring correctness.

## Required qualification gates

Any downstream project that claims seamless edited terrain must run gates in
these categories:

- authoritative sample persistence after edit, movement, restart, and snapshot
  compaction;
- edit-during-load and reload tests proving late-loaded chunks use edited
  state, not stale base terrain;
- rendered mesh probes for open edges, nonmanifold edges, zero-area triangles,
  and sky-leak rays around player-like tunnels and surface digs;
- close/mid/far/return movement tests proving edited shapes do not restore,
  disappear, or change harshly for the chosen profile;
- multi-site edit retention tests with edits in separated areas and viewer
  travel between them;
- startup and replacement tests proving human-visible terrain is not cleared
  before replacement render/collision is available;
- map-editor exact-region bake/reopen validation before editor-generated worlds
  are called production ready.

## Current implementation boundary

The current runtime keeps recent edit-retention zones as synthetic planner
viewers. Current defaults are:

- 256 remembered edit-retention zones;
- newest 32 zones remain active even without a nearby real viewer;
- 64 m merge distance between nearby edit zones;
- one LOD0 root-radius chunk around each retained edit zone;
- one to six LOD0 chunks of refinement around each retained edit zone.

When the full retention plan is too expensive, runtime planning degrades by
keeping newest/visible zones first and reducing retention refinement before
dropping retention entirely. This avoids the known all-or-nothing retention
collapse, but it is still a bounded behavior.

## Allowed release wording

A profile may claim authoritative edited terrain persistence only after sample,
reload, and compaction gates pass.

A profile may claim visually stable edited terrain only after the matching
close/mid/far/return and multi-site gates pass with the intended camera
distances, active chunk capacity, viewer radius, movement speed, and application
budgets.

A profile must not claim "all edits are full-resolution from every distance"
unless it has an explicit exact-global-edit-visibility or editor-baked
multi-LOD validation gate proving that claim.
