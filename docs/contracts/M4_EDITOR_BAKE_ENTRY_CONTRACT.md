# M4 Editor Bake Entry Contract

Status: normative M4 component

The Godot editor plugin exposes:

```text
Tools > World Transvoxel: Bake Dense Volume
```

This is intentionally thin scaffolding. It reads typed project settings,
constructs the same addon-owned `tools/wt_bake.py` command used at the command
line, and starts it as a separate process. GDScript does not load scalar
arrays, sample chunks, serialize pages, hash artifacts, or perform native bake
work.

## Project settings

Settings live under `world_transvoxel/bake/`:

```text
python_executable
script_path
density_path
material_path
key_path
output_path
origin
dimensions
spacing
source_revision
default_material
```

The script defaults to
`res://addons/world_transvoxel/tools/wt_bake.py`. Paths may use Godot project
paths and are globalized before process launch. Material input is optional.
Release-native tools live under `res://addons/world_transvoxel/bin/`, so the
installed addon does not depend on repository-root build or script folders.

Dimensions and spacing must be positive, source revision nonnegative, and
default material must fit `uint16_t`. Missing required paths or invalid values
fail before process creation.

The editor process is asynchronous. Completion and artifact validation remain
the storage/bake tool's responsibility; the editor frame thread does not run
the bake loop or wait for it.

## Evidence

`tests/godot/m4_editor_bake_test.gd` proves argument construction and invalid
configuration rejection on Godot 4.6.3 and 4.7. The full editor-load matrix
also parses and activates the plugin.

The editor and command-line entry points therefore share one Python/native
implementation rather than diverging serializers or sampling behavior.
