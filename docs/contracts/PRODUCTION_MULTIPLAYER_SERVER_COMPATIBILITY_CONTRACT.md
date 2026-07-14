# Production multiplayer and dedicated-server compatibility contract

Status: active design constraint for all downstream terrain/gameworld work.
This is not a claim that multiplayer gameplay is implemented today.

World Transvoxel must remain compatible with future multiplayer, large dedicated
servers, and persistent worlds. Terrain choices made for local play must not
force a later rewrite of authority, storage, streaming, or edit semantics.

## Research basis

Godot's official networking documentation recommends server-authoritative logic
for gameplay-critical decisions, validation of RPC arguments before applying
state, and rate limits for frequently triggered actions:

- https://docs.godotengine.org/en/stable/tutorials/networking/high_level_multiplayer.html#secure-multiplayer-design

Godot's high-level multiplayer API treats `"authority"` RPCs as callable only
by the multiplayer authority, with the server as the default authority:

- https://docs.godotengine.org/en/stable/tutorials/networking/high_level_multiplayer.html

Godot dedicated servers are expected to run with the headless display server and
Dummy audio driver, either through `--headless` or a dedicated server export:

- https://docs.godotengine.org/en/stable/tutorials/export/exporting_for_dedicated_servers.html
- https://docs.godotengine.org/en/stable/tutorials/editor/command_line_tutorial.html

The Transvoxel basis remains smooth voxel terrain generated from volumetric
voxel data, with render and collision meshes derived from that data:

- https://transvoxel.org/
- https://github.com/EricLengyel/Transvoxel

## Authority model

The authoritative terrain/world state is server-owned.

Clients may send intent:

- movement/viewer-interest updates;
- edit requests, such as carve, construct, or paint;
- map-editor commands when explicitly allowed by server policy;
- requests for streamed terrain state or validation data.

Clients must not author:

- authoritative density pages;
- authoritative material pages;
- source or world revisions;
- edit journal records;
- snapshot or compaction outputs;
- chunk ownership, retention, LOD plan, mesh, or collision state used as
  authority.

The server validates every gameplay-critical terrain request before mutating
world state. Validation includes at minimum:

- peer identity and permission;
- command schema and finite numeric values;
- requested shape, radius, material, and affected bounds;
- source/world revision compatibility;
- rate limits and per-peer budget;
- world bounds and protected regions;
- storage and runtime queue capacity.

Accepted edits commit as ordered transactions with a monotonic world revision.
Rejected edits do not partially mutate authoritative density/material samples.

## Data separation

The terrain source of truth is:

- source revision;
- world revision;
- density pages;
- material pages;
- durable edit journal;
- durable snapshots/compactions.

The network replication layer may transmit:

- revision numbers;
- validated edit transaction IDs and compact edit payloads;
- page/chunk hashes;
- page snapshots or delta snapshots;
- viewer-interest acknowledgements;
- correction messages for stale clients.

The following remain discardable client presentation data:

- render meshes;
- transition meshes;
- collision meshes;
- shader/material overrides;
- LOD fade or replacement state;
- local debug probes and screenshots.

If client presentation disagrees with authoritative samples, authoritative
samples win.

## Dedicated-server requirements

A dedicated server must be able to run terrain authority without a GPU, window,
human player, texture import dependency, or render/collision presentation path.

Server-compatible terrain work must keep these roles independent:

- authority: density/material storage, edit validation, journals, snapshots,
  world revisions, authoritative queries;
- streaming: viewer-interest planning, bounded queueing, page/chunk demand;
- presentation: client render meshes, client collision meshes, materials,
  lighting, and human visual diagnostics.

GPU compute, shaders, material import, and visual smoothing may accelerate or
improve clients, but they cannot be required for server correctness.

## Large-server requirements

Large multiplayer servers must not rely on global full-world scans in the hot
path. Standard terrain systems must preserve:

- bounded active chunk/page sets per viewer or interest group;
- explicit queue capacities and rejection behavior;
- event-driven edit invalidation;
- source/world revision tagging on work products;
- deterministic stale-result rejection;
- storage paths suitable for persistent world data;
- bounded snapshot/compaction work that can run side-by-side with the active
  world.

Viewer interest is a client/server streaming concern. It must not determine
what terrain exists authoritatively. A chunk can be unloaded from a client while
the server still owns the world state and edit history for that region.

## Multiplayer edit semantics

For every accepted terrain edit:

- the server creates or accepts a unique transaction identity;
- the server serializes the edit against the current world revision;
- all affected authoritative pages and dependent LOD/support pages are
  invalidated from authoritative samples, not from client meshes;
- clients receive either the committed edit/revision or later authoritative
  streamed state;
- stale client edits are rejected or rebased explicitly; they are never applied
  silently against the wrong revision.

For reconnect or late join:

- the client starts from a server-approved snapshot or source revision;
- the client replays or receives committed edits after that snapshot;
- visual meshes are rebuilt from the resulting authoritative samples.

## Required current primitives

The Godot-facing API must continue to expose these server-compatible primitives:

- `get_world_source_revision()`;
- `get_world_revision()`;
- `update_viewer(viewer_id, revision, position, radius_chunks, maximum_lod)`;
- `remove_viewer(viewer_id, revision)`;
- `begin_edit_transaction(author_id)`;
- `commit_edit_transaction(transaction)`;
- `request_authoritative_sample(...)`;
- `request_authoritative_samples(...)`;
- `request_world_compaction(...)`;
- `request_world_migration(...)`;
- `edit_committed(world_revision)`;
- `edit_failed(error)`;
- `authoritative_sample(s)_ready/failed`;
- `world_snapshot_ready/failed`;
- immutable chunk/sample snapshots;
- bounded runtime metrics.

Downstream wrappers may rename or simplify these APIs, but they must not remove
the underlying concepts.

## Non-negotiable rules

- Do not accept client mesh data as terrain truth.
- Do not make visual LOD state the saved world state.
- Do not make a dedicated server require render materials, texture import,
  crosshair/player UI, or GPU resources for authoritative terrain.
- Do not apply unvalidated edit payloads received from clients.
- Do not let stale client revisions mutate current terrain silently.
- Do not implement multiplayer by synchronizing only screenshots, meshes, or
  visible chunk state.
- Do not claim multiplayer or dedicated-server readiness until explicit
  networked and headless gates exist.

## Future qualification gates

Before claiming multiplayer terrain readiness, downstream projects must add and
pass finite gates for:

- headless dedicated-server startup with no render resources required for
  authority;
- two simulated clients sending concurrent terrain edits to the same region;
- stale edit rejection or explicit rebase;
- disconnect/reconnect from snapshot plus journal;
- late join after many committed terrain edits;
- interest streaming for multiple clients without full-world scans;
- server-side rate limiting and bounds rejection for malicious edit requests;
- persistence across server restart;
- long-running dedicated-server soak with terrain edits and viewer movement.

These are future gates, but the current terrain architecture must not make them
impossible.
