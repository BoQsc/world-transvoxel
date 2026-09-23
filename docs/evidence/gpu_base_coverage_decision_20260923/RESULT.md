# GPU base-coverage architecture decision — 2026-09-23

The production GPU candidate is not qualified for seamless cold movement.
The finite 2 km by 2 km by 256 m game profile requests global coarse coverage,
but the native LOD planner admits at most 16 uncovered staging roots per pass
(`wt_balanced_lod_planner.cpp`). The candidate currently has bounded in-memory
page/mesh caches, not a persisted drawable base atlas. A camera can therefore
expose a root before page load, mesh extraction, and GPU activation complete.
The game's minimum-resource startup threshold does not prove camera-wide
coverage. This is a missing architecture layer rather than a queue-priority
problem.

The last bounded Vulkan trace in the game capture directory did not reach its
tunnel route. It recorded a 3.4 s forced camera-viewer update, a 765 ms GPU
reconciliation stage, and a 433 ms draw callback while cold work was active.
Those are diagnostic wall times from one run, not steady-state percentiles.
They rule out claiming current-frame cold coverage or a low-cost runtime.

The next implementation must bake exact coarse roots offline, validate a
versioned manifest and explicit empty certificates, upload the complete base
before exposing the finite world, and keep that base across fine LOD and edit
handoffs. First measure atlas size and upload cost; reject the design if it
cannot meet bounded memory, frame-time, and power budgets. Gameplay validation
then requires road-speed movement with no absent visible terrain, followed by
rapid edits with the same invariant.

No renderer or gameplay behavior changed in this decision checkpoint.
