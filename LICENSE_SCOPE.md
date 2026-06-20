# License Scope

Project-owned source code and documentation use the root 0BSD license unless a
file states otherwise.

The official Transvoxel lookup data and any unchanged or transformed
substantial portions of Eric Lengyel's upstream implementation are MIT and
must remain isolated under:

```text
addons/world_transvoxel/thirdparty/transvoxel_mit/
```

That directory must contain the upstream copyright and permission notice from
`LICENSES/MIT-Transvoxel.txt`.

No MIT table data may be copied into project-owned 0BSD source, generated
assets, tests, fixtures, or binary formats. Project-owned code accesses the MIT
backend through an adapter.

Removing `addons/world_transvoxel/thirdparty/transvoxel_mit/` and disabling the
MIT backend must remove all Transvoxel MIT code and data from a distribution.

Downloaded papers and external repositories under `references/downloaded/`
are local research material and are intentionally untracked.
