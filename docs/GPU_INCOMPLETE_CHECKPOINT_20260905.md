# Incomplete GPU checkpoint, 2026-09-05

Status: INCOMPLETE_NOT_QUALIFIED. This is a recovery point for interrupted work.

Parent authority commit: c967998aef5855545f7d841838e916c8430b606f.
The accompanying source delta restores recognition of RefreshEditLodRetention
when a capacity-blocked event is popped from the viewer-event queue. Without
that restoration it follows ordinary staging instead of the intended retained
edit refinement path. An indentation correction is also preserved.

This change is not qualified as a latency improvement. The latest saved
diagnostic runs still fail visual latency/divergence; one also waits 76 frames
for edit commit. Their runtime artifacts do not match the recorded pin.
No native rebuild or new native test run was performed for this recovery commit.

The companion world-transvoxel-integration-game checkpoint preserves the
working DLLs, existing pin, focused fixture correction, compressed raw reports,
actual binary hashes, and the completion plan in docs/GPU_COMPLETION_PLAN.md.
Those DLLs have unverified build provenance; the integration pin still names
c967998 and must not be treated as describing this checkpoint's source.

Next: reproduce the requeue behavior in a focused native regression; rebuild
debug/release from the exact selected source with at most three build jobs;
sync binaries and regenerate the pin together. Then test bounded admission of
committed-edit refinement. The desired-set preflight requires free job slots
for all added demands plus role promotions before applying the delta. Queue
priority alone cannot make space. Preserve transactionality, generation identity,
retained coverage, collision support, and CPU-default behavior.
