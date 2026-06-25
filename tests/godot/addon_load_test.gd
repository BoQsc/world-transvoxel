extends SceneTree


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	if not ClassDB.class_exists("WorldTransvoxelTerrain"):
		_fail("WorldTransvoxelTerrain was not registered")
		return

	var terrain: Object = ClassDB.instantiate("WorldTransvoxelTerrain")
	if terrain == null:
		_fail("WorldTransvoxelTerrain could not be instantiated")
		return

	if terrain.call("get_addon_version") != "1.0.5":
		_fail("unexpected addon version")
		return
	if terrain.call("get_milestone") != "PQ4":
		_fail("unexpected milestone")
		return
	if not terrain.call("is_mit_backend_available"):
		_fail("official MIT backend is unavailable")
		return
	if terrain.call("get_backend_id") != "transvoxel_mit_official":
		_fail("unexpected backend ID")
		return
	if terrain.call("get_backend_license") != "MIT":
		_fail("unexpected backend license")
		return
	if terrain.call("get_backend_upstream_revision") != \
			"51a494f03c5b024cd153b596bcc7152eb3cc93a6":
		_fail("unexpected upstream revision")
		return

	terrain.free()
	print("ADDON_LOAD_TEST_PASS")
	quit(0)


func _fail(message: String) -> void:
	push_error("ADDON_LOAD_TEST_FAIL: " + message)
	quit(1)
