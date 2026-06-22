@tool
extends EditorPlugin

const BakeCommand := preload("res://addons/world_transvoxel/editor/world_transvoxel_bake_command.gd")
const MENU_LABEL := "World Transvoxel: Bake Dense Volume"
const SETTING_PREFIX := "world_transvoxel/bake/"


func _enter_tree() -> void:
	_define_settings()
	add_tool_menu_item(MENU_LABEL, _start_dense_bake)


func _exit_tree() -> void:
	remove_tool_menu_item(MENU_LABEL)


func _start_dense_bake() -> void:
	var settings := {
		"python_executable": ProjectSettings.get_setting(
			SETTING_PREFIX + "python_executable"
		),
		"script_path": ProjectSettings.get_setting(SETTING_PREFIX + "script_path"),
		"density_path": ProjectSettings.get_setting(SETTING_PREFIX + "density_path"),
		"material_path": ProjectSettings.get_setting(SETTING_PREFIX + "material_path"),
		"key_path": ProjectSettings.get_setting(SETTING_PREFIX + "key_path"),
		"output_path": ProjectSettings.get_setting(SETTING_PREFIX + "output_path"),
		"origin": ProjectSettings.get_setting(SETTING_PREFIX + "origin"),
		"dimensions": ProjectSettings.get_setting(SETTING_PREFIX + "dimensions"),
		"spacing": ProjectSettings.get_setting(SETTING_PREFIX + "spacing"),
		"source_revision": ProjectSettings.get_setting(
			SETTING_PREFIX + "source_revision"
		),
		"default_material": ProjectSettings.get_setting(
			SETTING_PREFIX + "default_material"
		),
	}
	var command: Dictionary = BakeCommand.build(settings)
	if not command["ok"]:
		push_error("World Transvoxel bake: " + command["error"])
		return
	var process_id := OS.create_process(
		command["executable"],
		command["arguments"],
		false
	)
	if process_id <= 0:
		push_error("World Transvoxel bake process could not be started.")
		return
	print("World Transvoxel bake started with process ID ", process_id)


func _define_settings() -> void:
	_define_setting("python_executable", "python", TYPE_STRING)
	_define_setting(
		"script_path",
		"res://addons/world_transvoxel/tools/wt_bake.py",
		TYPE_STRING
	)
	_define_setting("density_path", "", TYPE_STRING)
	_define_setting("material_path", "", TYPE_STRING)
	_define_setting("key_path", "", TYPE_STRING)
	_define_setting("output_path", "", TYPE_STRING)
	_define_setting("origin", Vector3i(-1, -1, -1), TYPE_VECTOR3I)
	_define_setting("dimensions", Vector3i(19, 19, 19), TYPE_VECTOR3I)
	_define_setting("spacing", 1, TYPE_INT)
	_define_setting("source_revision", 1, TYPE_INT)
	_define_setting("default_material", 0, TYPE_INT)


func _define_setting(name: String, default_value: Variant, type: int) -> void:
	var path := SETTING_PREFIX + name
	if not ProjectSettings.has_setting(path):
		ProjectSettings.set_setting(path, default_value)
	ProjectSettings.set_initial_value(path, default_value)
	ProjectSettings.add_property_info({
		"name": path,
		"type": type,
	})
