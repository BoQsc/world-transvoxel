# Production Runtime Configuration Contract

Status: complete foundation for production qualification PQ0

## Purpose

`WorldTransvoxelConfig` is the versioned Godot `Resource` that records every
construction-time bound already required by the M5 native services. It is
attached to `WorldTransvoxelTerrain`, but it does not start a world or allocate
runtime services. Startup remains the next PQ0 unit.

The C++ authority is the fixed-width `WtRuntimeConfig`. Godot values convert to
that structure and pass the same native validator before lifecycle startup can
consume them.

## Schema 1

| Property | Default | Valid range |
| --- | ---: | ---: |
| `active_chunk_capacity` | 256 | 1..65,536 |
| `viewer_capacity` | 8 | 1..1,024 |
| `demand_capacity_per_viewer` | 4,096 | 1..65,536 |
| `storage_request_capacity` | 256 | 1..65,536 |
| `storage_completion_capacity` | 256 | 1..65,536 |
| `encoded_page_entry_capacity` | 256 | 1..65,536 |
| `encoded_page_byte_capacity` | 67,108,864 | 1..1,073,741,824 |
| `decoded_page_entry_capacity` | 128 | 1..65,536 |
| `decoded_page_byte_capacity` | 67,108,864 | 1..1,073,741,824 |
| `mesh_entry_capacity` | 128 | 1..65,536 |
| `mesh_byte_capacity` | 134,217,728 | 1..1,073,741,824 |
| `render_entry_capacity` | 128 | 1..65,536 |
| `render_byte_capacity` | 134,217,728 | 1..1,073,741,824 |
| `collision_entry_capacity` | 64 | 1..65,536 |
| `collision_byte_capacity` | 67,108,864 | 1..1,073,741,824 |
| `trace_event_capacity` | 65,536 | 1..262,144 |
| `render_apply_budget` | 4 | 0..128 |
| `collision_apply_budget` | 2 | 0..128 |
| `collision_activation_distance` | 96.0 | finite and nonnegative |
| `collision_deactivation_distance` | 128.0 | finite and at least activation |

The product of viewer and per-viewer demand capacities may not exceed 65,536.
This keeps the configured total demand bound compatible with the M5
desired-set contract.

Schema version is read-only and currently `1`. Unknown schemas fail instead of
being reinterpreted.

## Mutation and startup policy

Resource setters preserve the exact supplied value and emit Godot's `changed`
notification. They do not silently clamp invalid values. `is_valid()` and
`get_validation_error()` expose deterministic validation, and future startup
must reject a missing or invalid resource before creating workers, queues,
caches, or scene resources.

The configuration becomes immutable runtime input when startup begins. A later
editor mutation does not silently resize live native containers; applying a
new configuration requires a controlled stop and restart.

`WorldTransvoxelTerrain` requires an explicit resource through its
`configuration` property and reports missing or invalid configuration. It
does not instantiate a hidden per-node default Resource during class-default
inspection. This attachment alone makes no running-world claim.

## Proof

`test_wt_production_config.cpp` locks defaults, schema identity, eleven
rejection categories, deterministic validation messages, and matching hashes
in debug and release builds.

`production_config_test.gd` proves resource registration, default values,
explicit terrain attachment, total-demand rejection, collision-distance
rejection, and missing-resource rejection on Godot 4.6.3 and 4.7 using both
addon builds.

The active entry point is:

```console
python scripts/test_production_qualification.py
```

That suite includes the complete M5-through-M0 regression chain. Passing this
contract means the first PQ0 foundation is complete, not that the addon is
production-ready.
