# Publication dependency subsystem replacement

Status: native correctness checks pass; GPU gameplay remains unqualified.
Baseline native `674ecc3`, integration checkpoint `6271af5`.

The old closure builder combined hierarchy overlap lookup with a full retirement
scan for each replacement and expansion cursors into a sorted, growing vector.
The replacement is a directed dependency traversal over immutable integer AABB
indexes. A replacement requires overlapping retired coverage and old face
neighbors whose LOD gap exceeds one; a retirement requires all overlapping
replacements. Each selected vertex is expanded once. Only final output is sorted.

The GPU session retains the two indexes in a typed native dependency graph.
Exact replacement/retirement key-vector comparison invalidates the affected
index. Readiness, generation identity, active content and transition masks are
queried anew. GPU session end releases the graph. No terrain format, lookup
table, meshing rule, collision rule, or atomic publication gate is relaxed.

The first prototype rebuilt both indexes per query and regressed larger cases.
That observation is retained in `comparison_rebuilt_each_query.json`. Reusing
unchanged membership is essential to the integrated design.

## Bounded experiments

Twenty-five repeated debug queries after one warmup, three CPU affinity, before
and after executables, deterministic sparse publication layouts. These measure
native cohort selection only, not full-frame or gameplay performance.

| Retirement entries | Previous median us | Retained-index median us | Exact output |
| ---: | ---: | ---: | --- |
| 64 | 67.8 | 71.4 | Equal |
| 512 | 167.5 | 71.1 | Equal |
| 4,096 | 730.8 | 68.5 | Equal |

The first index construction is excluded from these warm timings and still
costs work when membership changes. Small cases show no established improvement.
Selected replacements, retirements, waiting masks, build status, and coverage
verdict match the previous executable. Compressed replay inputs are retained.

Independent integer all-pairs reference checks pass for 250 randomized layouts
and coordinate limits. Authoritative coverage checks pass for 180 partitions,
three authority predicates and six mutations. Eight local queries among 4,096
retirements test 32 leaf keys versus 32,768 all-pairs tests. Snapshot tests prove
100 unchanged updates retain indexes, replacement/retirement changes invalidate
independently, and 128 mask/readiness changes match fresh queries.

All seven native regression suites pass in debug and release: production
streaming, LOD streaming, lifecycle, edit replacement, application, publication
policy and GPU shadow. Full debug/release builds complete without compiler
warnings. Integration and gameplay evidence will be recorded by the consumer
after installing the exact committed binaries.
