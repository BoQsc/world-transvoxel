# GPU collision/capture decoupling

Mixed GPU-visual and CPU-collision generations may now finish collision
preparation while the bounded GPU capture queue is saturated. Immutable
pre-mesh page references remain owned by the page runtime in a bounded deferred
state. Capacity notification resumes GPU capture for the same generation in
priority order, without repeating CPU meshing or publishing a CPU visual.

The production saturation test covers synchronous and one-worker execution. It
holds every GPU capture slot, observes a nonempty collision payload and no render
payload, releases capacity, then observes exactly one GPU-resident placeholder
for the same generation. Each mode performs one mesh job and zero remeshes.

All seven selected native regression suites pass in debug and release. Gameplay
measurements are recorded in the integration repository after pinning the exact
native commit.
