# Research Paper Preservation

The repository preserves unmodified copies only when the distributed artifact
contains, or its authoritative landing page identifies, a license that permits
redistribution. The root 0BSD license does not apply to these papers.

PDF files are stored with Git LFS. After cloning, run `git lfs pull` to retrieve
their contents. `python tools/validate_research_papers.py` verifies every
preserved file against its recorded byte count and SHA-256 digest.

## Preserved PDFs

| Paper | Attribution | License | Repository path |
| --- | --- | --- | --- |
| Voxel-Based Terrain for Real-Time Virtual Simulations | Eric Lengyel, copyright 2010 | [CC BY-ND 3.0](https://creativecommons.org/licenses/by-nd/3.0/); unmodified | `references/preserved/papers/Lengyel-VoxelTerrain.pdf` |
| McGrids: Monte Carlo-Driven Adaptive Grids for Iso-Surface Extraction | Daxuan Ren, Hezi Shi, Jianmin Zheng, and Jianfei Cai | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/); unmodified | `references/preserved/papers/McGrids-2024.pdf` |
| TetWeave: Isosurface Extraction using On-The-Fly Delaunay Tetrahedral Grids for Gradient-Based Mesh Optimization | Alexandre Binninger, Ruben Wiersma, Philipp Herholz, and Olga Sorkine-Hornung | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/); unmodified | `references/preserved/papers/TetWeave-2025.pdf` |
| Isosurface Extraction for Signed Distance Functions using Power Diagrams | Maximilian Kohlbrenner and Marc Alexa, copyright 2025 | Creative Commons Attribution, as stated in the distributed paper; unmodified | `references/preserved/papers/Power-Isosurfacing-2025.pdf` |

## Link-and-hash-only references

The following local research copies are deliberately not vendored. Their
identity remains reproducible through the URL, byte count, and SHA-256 digest
in `manifest.json`.

- `Dual-Contouring-Hermite-Data.pdf`: no redistribution license was identified
  in the artifact or authoritative source.
- `Wald-AMR-IsoSurface-2020.pdf`: the arXiv submission uses arXiv's
  non-exclusive distribution license, which grants distribution rights to
  arXiv rather than to downstream repositories.
- `Power-Diagram-Adaptive-Isosurface-2025.pdf`: the arXiv submission uses the
  same arXiv-specific non-exclusive distribution license.
- `Dual-Contouring-Signed-Distance-2026.pdf`: the PDF states CC BY-NC-ND 4.0,
  while its embedded metadata and arXiv landing page identify CC BY-NC-SA 4.0.
  The binary stays link-only until the authors or publisher resolve that
  conflict.

## Preservation policy

- Never edit a NoDerivatives paper in place.
- Do not infer a paper's license from an accompanying code repository.
- Record the exact artifact hash before vendoring or refreshing a paper.
- Treat a changed upstream hash as a new review event, not an automatic update.
- Keep unverified or conflicting-license artifacts link-and-hash-only.
