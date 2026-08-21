# Static lake authority contract

Status: normative CPU terrain contract

World Transvoxel static lakes use a secondary signed-density field. They are
volumetric terrain data, but they are not a fluid simulation.

## Authority

The authoritative state consists of two independent fields at every sample:

- terrain density and material;
- static-water density.

Terrain depth-occludes water. It does not erase hidden water and is not
boolean-intersected into the water field. This keeps the water shoreline stable
across terrain LODs and allows ordered terrain edits to expose or cover water
without creating a second geometry authority.

The `four_biomes_lakes_caves_roads` reference source authors three fixed,
gravity-aligned elliptical lake fields. Each has a fixed horizontal footprint
and water level. The terrain generator authors a matching smooth basin
depression, then applies road grading and cave subtraction to terrain. The
secondary water field is sampled separately from that final terrain density.
In the untouched reference world, the basin must rise above the water level
before the water field reaches its lateral closure. The visible shoreline is
therefore the gradual terrain/free-surface intersection; the complete hidden
water boundary must not appear as a vertical wall in open air.

## Ordered terrain interaction

Edit journals remain strictly revision and sequence ordered. Static-water
interaction follows these deterministic rules:

- construction can cover water, but does not delete its secondary density;
- excavation into a negative static-water sample exposes water and assigns the
  static-water material to the newly empty terrain sample;
- placing static water unions the brush into the existing water field;
- removing static water subtracts the brush from the water field without
  changing terrain density or material;
- later construction and excavation repeat the same cover/reveal behavior;
- no operation implies drainage, displacement, pressure, connectivity, flow,
  evaporation, or automatic basin refilling.

Procedural roads are terrain fields. A road or shoulder can therefore occlude
water where its terrain density is solid. The current reference road graph does
not cross a lake with its asphalt corridor; it only approaches the gravel lake
closely enough for a shoulder to overlap the lake footprint. This reference map
must not be cited as proof of bridge, causeway, culvert, or road-water behavior.

## Rendering

Generated and edited water retain the complete Transvoxel boundary represented
by the secondary density field. The render payload must not classify generated
water by provenance, discard non-upward triangles, flatten vertices, or replace
the field with a hand-authored oval surface mesh. A flat top is a valid part of
a gravity-aligned volume; its shoreline and submerged boundary remain geometry
and become visible wherever terrain does not depth-occlude them.

Terrain controls water visibility through ordinary depth occlusion. It is not a
second water geometry source and must not be used to destructively clip the
secondary field during meshing.

## Qualification

The native suite must prove:

- exposed lake samples are terrain air with negative water density;
- basin-floor samples remain solid while retaining hidden water density;
- samples above the water level and outside the footprint are outside water;
- nearby road terrain and lake water preserve their separate fields;
- construction preserves hidden water density;
- later excavation re-exposes that water;
- reversing construction and excavation order produces the corresponding
  deterministic final terrain state;
- procedural free-surface elevation and footprint remain stable across LODs;
- procedural shoreline volume survives render-payload construction across LODs;
- procedural lateral closure remains occluded by solid terrain in the untouched
  reference world;
- rendered generated water is the unmodified Transvoxel water payload.

Dynamic hydrology is a separate future system. It must not be inferred from
this static-lake contract or patched into terrain meshing.
