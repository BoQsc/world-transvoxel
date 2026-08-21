# Addon License Scope

Project-owned addon source, tooling, and documentation are 0BSD.

The official Transvoxel source and lookup data under
`thirdparty/transvoxel_mit/` are MIT-licensed and retain Eric Lengyel's
copyright and permission notice. Runtime binaries compiled from that code
remain subject to the MIT notice even when a binary-only distribution omits
`Transvoxel.cpp`.

This addon is therefore a mixed 0BSD/MIT distribution while the official
backend is included. A binary-only runtime artifact must retain the MIT
`LICENSE` and provenance notice. Removing the source directory alone does not
remove the MIT code from compiled binaries.

The official tables must not be copied into project-owned source, generated
fixtures, binary formats, or an independent 0BSD backend.
