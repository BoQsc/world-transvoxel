# M5 Transition Page Source Contract

Status: implemented deterministic ownership and seam contract

## Purpose

Schema-1 pages store a 19-cubed sample lattice at the page LOD's spacing. A
coarse page therefore owns every sample required by its regular cells, but it
does not independently contain the half-spacing samples required by a
Transvoxel transition face.

The schema remains unchanged. Transition samples are supplied by explicitly
owned adjacent pages at the next finer LOD.

## Ownership rule

For a coarse chunk at LOD `L > 0`, each active transition face owns exactly
four support pages at LOD `L - 1`. Those pages are immediately outside the
coarse face and tile its two tangential axes.

`wt_transition_support_page_keys()` returns the canonical four keys for any
coarse key and face. It:

- rejects LOD0 and invalid faces;
- handles negative chunk coordinates without truncation ambiguity;
- rejects coordinate doubling that would exceed signed 32-bit page keys;
- returns keys in deterministic tangential-axis order.

A chunk with multiple transition faces owns the bounded union of those sets,
with a hard maximum of 24 support pages.

These are **sample dependencies** for meshing. They are not by themselves the
complete set of visible fine chunks required around an LOD edge or corner.
Desired-set ownership still determines which fine chunks render.

## Decoded-page sample source

`WtChunkPageSampleSource` holds one decoded primary page and up to 24 decoded
support pages. It:

- accepts only valid schema-1 decoded pages;
- requires every support page to have the primary source revision;
- accepts only a canonical support key;
- keeps support pointers in canonical chunk-key order;
- rejects duplicates and incomplete transition masks;
- resolves only exact stored lattice samples;
- compares every overlapping decoded value and fails on density/material
  disagreement;
- performs no interpolation and invents no missing sample.

The caller owns page lifetime and must keep the primary/support pages pinned
through the complete meshing call.

## Gradient spacing

Regular-cell central differences use the regular page spacing `2^L`.
Transition-cell central differences use the support-page spacing `2^(L-1)`.
The cell-sample cache is cleared between regular and transition phases so a
point shared by both phases cannot retain a gradient computed at the wrong
spacing.

This change updates the M2 chunk hash while preserving every same-LOD seam,
single-face transition seam, multi-face edge, and corner closure test.

## Evidence

`tests/native/test_wt_m5_page_transition.cpp` proves:

- exact support keys for all six faces and negative coordinates;
- safe rejection of LOD0 and overflowing support requests;
- all six decoded-page transition faces at coarse LOD2 match the four decoded
  LOD1 neighbors on the full-resolution contour;
- every decoded mesh exactly matches the equivalent direct procedural source;
- a decoded three-face LOD corner and its visible fine neighbors form one
  closed sphere;
- missing, unrelated, duplicate, revision-mismatched, and overlapping
  conflicting support pages fail safely.

The locked debug/release aggregate hash is:

```text
7717f75423306cca
```

The integrated performance workload loads 17 real content-addressed files:
four LOD0 regular pages, one LOD1 three-face coarse page, and twelve required
LOD0 support pages. The transition mesh is produced only from those decoded
pages through the official MIT backend.

## Runtime integration

`M5_PAGE_MESHING_RUNTIME_CONTRACT.md` completes the production integration.
Real jobs derive these support keys, request cache misses from asynchronous
storage, pin every decoded dependency through official MIT meshing, release
pins on completion/failure/cancellation, and retain dependency identities for
event-driven coarse-generation invalidation. Missing support and overlap
conflicts fail through typed runtime status/metrics. Desired-set removal and
edit replacement notify the page-meshing owner through a narrow interface.
