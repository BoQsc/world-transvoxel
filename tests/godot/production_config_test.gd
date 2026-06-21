extends SceneTree


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	if not ClassDB.class_exists("WorldTransvoxelConfig"):
		_fail("WorldTransvoxelConfig was not registered")
		return
	var config: Resource = ClassDB.instantiate("WorldTransvoxelConfig")
	if config == null or config.call("get_schema_version") != 1:
		_fail("configuration schema is unavailable")
		return
	if not config.call("is_valid") or config.call("get_validation_error") != "ok":
		_fail("default configuration is invalid")
		return
	if config.get("active_chunk_capacity") != 256 or \
			config.get("viewer_capacity") != 8 or \
			config.get("render_apply_budget") != 4 or \
			config.get("collision_apply_budget") != 2:
		_fail("default configuration values changed")
		return

	var terrain: Node = ClassDB.instantiate("WorldTransvoxelTerrain")
	if terrain == null or terrain.call("is_configuration_valid") or \
			terrain.call("get_configuration_error") != "configuration is required":
		_fail("terrain did not require explicit configuration")
		return
	terrain.set("configuration", config)
	if terrain.get("configuration") != config or \
			not terrain.call("is_configuration_valid"):
		_fail("terrain did not retain the configuration resource")
		return

	config.set("viewer_capacity", 1024)
	config.set("demand_capacity_per_viewer", 65536)
	if config.call("is_valid") or terrain.call("is_configuration_valid") or \
			config.call("get_validation_error") != \
			"viewer and demand capacities exceed 65536 total demands":
		_fail("total demand overflow was not rejected")
		return
	config.set("viewer_capacity", 8)
	config.set("demand_capacity_per_viewer", 4096)
	config.set("collision_activation_distance", 129.0)
	config.set("collision_deactivation_distance", 128.0)
	if config.call("is_valid") or config.call("get_validation_error") != \
			"collision distances must be finite, nonnegative, and ordered":
		_fail("inverted collision distances were not rejected")
		return

	terrain.set("configuration", null)
	if terrain.call("is_configuration_valid") or \
			terrain.call("get_configuration_error") != "configuration is required":
		_fail("missing configuration was not rejected")
		return
	terrain.free()
	print("PRODUCTION_GODOT_CONFIG_PASS schema=1")
	quit(0)


func _fail(message: String) -> void:
	push_error("PRODUCTION_GODOT_CONFIG_FAIL: " + message)
	quit(1)
