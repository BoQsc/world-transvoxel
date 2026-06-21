extends SceneTree

const BakeCommand := preload(
	"res://addons/world_transvoxel/editor/world_transvoxel_bake_command.gd"
)


func _initialize() -> void:
	var command: Dictionary = BakeCommand.build({
		"python_executable": "python",
		"script_path": "res://tools/wt_bake.py",
		"density_path": "res://density.f32",
		"material_path": "res://materials.u16",
		"key_path": "res://keys.txt",
		"output_path": "res://baked",
		"origin": Vector3i(-1, -2, -3),
		"dimensions": Vector3i(35, 19, 19),
		"spacing": 1,
		"source_revision": 7,
		"default_material": 4,
	})
	if not command["ok"]:
		_fail("valid editor bake settings were rejected")
		return
	var arguments: PackedStringArray = command["arguments"]
	if command["executable"] != "python" or \
			arguments.find("--materials") < 0 or \
			arguments.find("35") < 0 or \
			arguments.find("7") < 0:
		_fail("editor bake command arguments mismatch")
		return
	var invalid: Dictionary = BakeCommand.build({
		"python_executable": "python",
		"script_path": "res://tools/wt_bake.py",
		"density_path": "res://density.f32",
		"key_path": "res://keys.txt",
		"output_path": "res://baked",
		"dimensions": Vector3i.ZERO,
		"spacing": 1,
		"source_revision": 7,
		"default_material": 0,
	})
	if invalid["ok"]:
		_fail("invalid editor bake dimensions were accepted")
		return
	print("M4_EDITOR_BAKE_PASS")
	quit(0)


func _fail(message: String) -> void:
	push_error("M4_EDITOR_BAKE_FAIL: " + message)
	quit(1)
