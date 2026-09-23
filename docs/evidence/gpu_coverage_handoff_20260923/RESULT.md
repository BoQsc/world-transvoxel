# GPU coverage handoff checkpoint — 2026-09-23

The GPU candidate is not qualified. A render-sink placeholder for the same
generation but a changed transition mask previously cleared the active GPU
replacement immediately. The old coverage is now retained as the pending
placeholder waits for the atomic GPU cohort commit. The same-layout edit fast
path also no longer treats a missing active GPU replacement plus transition
mask zero as proof that a chunk was empty. Such an edit enters the full
topology/coverage cohort.

Both native debug and release DLLs built. The publication-policy, production
LOD-streaming, and GPU-meshing-shadow native tests passed in both
configurations. `test_wt_m3_application` reported 15 failures in both
configurations; SCons reported those executables up to date and this checkpoint
did not change their compiled inputs. This is an existing failing baseline,
not a pass. `git diff --check` passed.

This checkpoint establishes a stricter coverage handoff rule. It does not prove
that every cold visible region has a base mesh, that edited terrain meets a
two-frame target, or that GPU gameplay is free of holes. Those require an
in-engine candidate run and the larger base-coverage streaming redesign.
