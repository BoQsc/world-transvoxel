@tool
extends RefCounted


static func build(settings: Dictionary) -> Dictionary:
	var python_executable: String = str(settings.get("python_executable", ""))
	var script_path: String = str(settings.get("script_path", ""))
	var density_path: String = str(settings.get("density_path", ""))
	var material_path: String = str(settings.get("material_path", ""))
	var key_path: String = str(settings.get("key_path", ""))
	var output_path: String = str(settings.get("output_path", ""))
	var origin: Vector3i = settings.get("origin", Vector3i.ZERO)
	var dimensions: Vector3i = settings.get("dimensions", Vector3i.ZERO)
	var spacing: int = int(settings.get("spacing", 0))
	var source_revision: int = int(settings.get("source_revision", -1))
	var default_material: int = int(settings.get("default_material", -1))

	if python_executable.is_empty():
		return _failure("Python executable is empty.")
	if script_path.is_empty() or density_path.is_empty() or \
			key_path.is_empty() or output_path.is_empty():
		return _failure("Bake script, density, keys, and output paths are required.")
	if dimensions.x <= 0 or dimensions.y <= 0 or dimensions.z <= 0:
		return _failure("Bake dimensions must be positive.")
	if spacing <= 0 or source_revision < 0:
		return _failure("Bake spacing and source revision are invalid.")
	if default_material < 0 or default_material > 65535:
		return _failure("Default material must fit uint16.")

	var arguments := PackedStringArray([
		ProjectSettings.globalize_path(script_path),
		ProjectSettings.globalize_path(density_path),
		ProjectSettings.globalize_path(key_path),
		ProjectSettings.globalize_path(output_path),
		"--origin",
		str(origin.x),
		str(origin.y),
		str(origin.z),
		"--dimensions",
		str(dimensions.x),
		str(dimensions.y),
		str(dimensions.z),
		"--spacing",
		str(spacing),
		"--source-revision",
		str(source_revision),
		"--default-material",
		str(default_material),
		"--configuration",
		"template_release",
	])
	if not material_path.is_empty():
		arguments.append("--materials")
		arguments.append(ProjectSettings.globalize_path(material_path))
	return {
		"ok": true,
		"error": "",
		"executable": python_executable,
		"arguments": arguments,
	}


static func _failure(message: String) -> Dictionary:
	return {
		"ok": false,
		"error": message,
		"executable": "",
		"arguments": PackedStringArray(),
	}
