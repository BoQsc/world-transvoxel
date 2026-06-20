# M3 Render, Collision, and Application Contract

Status: normative for M3 implementation

## Execution boundary

Worker threads may build immutable render and collision payloads and submit
them to bounded queues. Only the Godot main thread may consume those queues,
create resources, mutate scene objects, or change readiness records.

No Godot object, `Variant`, string-key payload, or scene pointer crosses a
worker queue. Queue ownership is transferred with `shared_ptr<const Payload>`;
the payload cannot be mutated after submission.

## Render payload

`WtRenderPayload` combines one M2 chunk result in deterministic order:

1. regular buffer;
2. transition buffers from face bit 0 through face bit 5.

Positions and normals remain chunk-local floats. The payload retains the
signed 64-bit world origin, chunk key, generation token, and categorical
`uint16_t` material. Inactive transition buffers must be empty. Invalid
indices, non-finite attributes, zero-area triangles, invalid origins, and
capacity overflow fail the complete build and clear output.

Maximum combined payload sizes are 67,584 vertices and 116,736 indices.

## Collision sanitation

Collision is generated as an unindexed triangle-face array from the immutable
render payload. Visual and collision outputs are therefore separate even when
they begin from the same authoritative topology.

For each triangle:

```text
shape_ratio_squared = |cross(b-a, c-a)|^2 / max_edge_length^4
```

- zero edge length or zero area is degenerate and is removed;
- a nonzero triangle is thin and removed when
  `shape_ratio_squared <= 1e-12` by default;
- winding and triangle order are preserved for retained faces;
- invalid indices or non-finite positions fail and clear the complete output;
- removed-degenerate and removed-thin counts are recorded separately.

The ratio is scale-independent. `1e-12` corresponds approximately to a
minimum altitude-to-longest-edge ratio of `1e-6`; it is a locked correctness
baseline, not a final production physics-performance budget.

## Collision distance policy

The M3 baseline uses hysteresis:

```text
not currently required: activate at distance <= 96 base units
currently required:     retain through distance <= 128 base units
```

Both distances are validated finite and nonnegative, and deactivation must be
at least activation. These defaults prevent boundary oscillation in the M3
integration fixture. Representative production distances remain an M5 budget
decision.

## Bounded application and readiness

Render and collision use separate mutex-protected bounded FIFO queues. Queue
overflow is explicit and counted. `WtChunkApplicationService` owns a bounded,
sorted set of expected chunk generations and independent visual/collision
readiness bits.

- Expecting a newer generation resets both readiness states.
- Older expectations are rejected.
- A queued payload is compared with the expected generation before a sink is
  called.
- Missing, superseded, or forgotten payloads are stale and discarded.
- Collision results are discarded when collision is no longer required.
- Stale, unrequired, and sink-failed entries consume application budget so a
  frame cannot perform unbounded cleanup.
- A chunk is fully ready when visual output is ready and collision output is
  either ready or not required.
- A zero budget performs no queue work or record scan.

Queue submission is worker-safe. Expectations, application, readiness access,
and Godot sinks are main-thread operations.

## Current proof and remaining gate

`tests/native/test_wt_m3_application.cpp` proves deterministic render-buffer
combination, inactive-face rejection, collision sanitation and metrics,
distance hysteresis, bounded queues and records, independent readiness,
pre-sink stale rejection, frame budgets, and 1,000 supersession cycles with
bounded state in debug and optimized builds.

M3 is not complete at this contract stage. Remaining work is Godot `ArrayMesh`
and concave collision resource creation, chunk resource replacement/removal,
main-thread integration, and the moving-viewer/teardown test required by the
roadmap exit condition.
