# Full-quality foreground planning

The GPU viewer activation policy projects the complete 3x3x3 interaction neighborhood directly to its target resolution, with complete sibling coverage and 2:1 balancing. It no longer waits for intermediate LOD publication or a coarse edited surface before requesting fine geometry. Atomic publication and collision safety remain unchanged.

Validation: all seven native suites passed in debug and release. The focused dense-hierarchy case requests all 27 LOD0 focus keys in one plan (129 total leaves), retains a distant coarse root, and rejects insufficient capacity. No intermediate publications are required. GPU gameplay latency remains to be measured; this is not an instant-editing claim.
