# Production terrain volume contract

Status: active downstream contract for terrain and gameworld repositories.

World Transvoxel provides the native voxel terrain backend. Downstream terrain
addons and games that claim Transvoxel terrain must use it as a signed
density/material volume, not as a heightmap-only mesh pipeline.

## Research basis

Primary references:

- https://transvoxel.org/
- https://transvoxel.org/Lengyel-VoxelTerrain.pdf
- https://github.com/EricLengyel/Transvoxel

The official Transvoxel description defines the algorithm as seamless stitching
between meshes generated from voxel data at different resolutions. Lengyel's
dissertation defines voxel terrain as a scalar function over a 3D grid, where
the zero set is the terrain surface, and contrasts that with height fields over
a 2D elevation grid.

## Standard requirement

A downstream Transvoxel terrain implementation must keep the following as the
source of truth:

- authoritative density samples;
- authoritative material samples;
- authoritative secondary static-water density samples when static water is
  present;
- 3D chunk/page identity including X, Y, Z, and LOD;
- edit journals or snapshots that replay into those samples;
- render, collision, and transition meshes derived from the samples.

Meshes are never the authoritative terrain. Screenshots, capping geometry,
debug overlays, fallback planes, and presentation layers are never valid terrain
state.

## Height-derived generation boundary

A generator may use a height function to create an initial surface, including a
flat baseline profile. That does not make the terrain heightmap terrain.

The generated result must still be a full signed volume:

- positive density means empty space;
- negative density means solid space;
- the zero crossing defines the surface;
- underground samples exist as real editable samples;
- digging and placing mutate the volume, not only the mesh.

## Required downstream claims

A terrain addon or gameworld repository may claim standard Transvoxel terrain
only after proving:

- bounded startup readiness with no missing native terrain after initial ready;
- close-range edits mutate authoritative samples and collision;
- edited samples survive movement, LOD replacement, save/reload, and revisit;
- close/mid/far/return movement does not restore edited terrain;
- downstream profile summaries expose horizontal and vertical volume bounds;
- underground support is explicit in profile summaries;
- human-visible artifacts are promoted to targeted gates.

## Current non-goals

This contract does not require:

- natural cave generation;
- ore veins;
- collapse simulation;
- dynamic water or lava simulation;
- vegetation;
- block buildings;
- planet-scale coordinate transforms;
- exact full-resolution visibility of every edit from every distance.

Static water may use a separate signed scalar field that is sampled, paged, and
meshed under the same spatial/LOD contract independently of the terrain scalar.
Terrain depth-occludes water beneath solid ground; it must not be boolean-
intersected into the water geometry because doing so makes the shoreline depend
on terrain LOD. A categorical material ID alone is not sufficient geometry for
a stable free surface. Dynamic fluid simulation remains a later system layered
on the same volume authority.

The render payload does not distinguish generated and edited static water for
geometry construction. Both retain the complete Transvoxel boundary represented
by the secondary scalar field, including non-upward surfaces when exposed.
Terrain controls visibility through depth occlusion; render-payload code must
not flatten, clip, or replace either source with separate presentation geometry.
