# Edit refinement correctness, 2026-09-05

This change fixes two independent ways to lose a committed edit's intended
refinement, and adds a regression for the refresh-kind correction inherited
from checkpoint `4578e73`.

1. A queued internal refresh borrowed an external viewer's ID. A subsequent
   viewer update could coalesce into that entry and discard the refresh.
   Internal refresh/staging entries are now excluded from external-event
   coalescing. External update/remove revision handling is unchanged.
2. A preferred multi-split refinement could temporarily produce neighboring
   leaves two LODs apart. The next iteration rejected this intermediate map
   before the final balancing pass. The existing bounded 2:1 balance operation
   now runs before the next selection. Intermediate states remain private;
   only the complete balanced plan can enter desired-set/publication handling.

The deterministic full-queue regression uses zero/one mesh-worker configurations,
an internal retry, and a later external viewer update. It checks that rejection
does not apply a partial plan, the refresh is preserved, and the external viewer
revision advances only when its own event is processed. Removing either the
inherited refresh classification or the new coalescing exclusion reproduces the
failure; restoring both passes. See `negative_controls.json` and paired logs.

The planner regression fails before and passes after in twelve cases: all six
face directions at the original and negative-coordinate locations. It requires
the exact independently constructed balanced target plan, including mandatory
support families. Existing background staging, collision continuity, retained
coverage, and LOD regressions remain active. The established LOD-streaming hash
is unchanged: `1a59569e2131a7aa07279004a8c2ce304278da658047c3aac8d56bb601ae87a3`.

These are correctness fixes, not a GPU performance qualification. Full runtime
measurements and their exact binary provenance are recorded in the integration
repository. The checkpoint's queue-admission hypothesis is not established:
the saved gameplay samples show dozens of queued jobs against thousands of
scheduler slots. No queue capacity, priority, retry budget, publication guard,
or GPU shader is changed here.
