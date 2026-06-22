extends SceneTree


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	var config: Resource = ClassDB.instantiate("WorldTransvoxelConfig")
	var terrain: Node = ClassDB.instantiate("WorldTransvoxelTerrain")
	if config == null or terrain == null:
		_fail("multi-LOD streaming classes could not be instantiated")
		return
	config.set("active_chunk_capacity", 40)
	config.set("viewer_capacity", 2)
	config.set("demand_capacity_per_viewer", 125)
	config.set("storage_request_capacity", 64)
	config.set("storage_completion_capacity", 64)
	config.set("encoded_page_entry_capacity", 40)
	config.set("decoded_page_entry_capacity", 40)
	config.set("mesh_entry_capacity", 40)
	config.set("render_entry_capacity", 40)
	config.set("collision_entry_capacity", 40)
	root.add_child(terrain)
	terrain.set("configuration", config)

	const fixture_root := "res://build/production-lifecycle-fixture"
	if not terrain.call(
		"start_world",
		fixture_root + "/transition.wtworld",
		fixture_root
	):
		_fail("hierarchical baked world startup was rejected")
		return
	if not await _wait_for_state(terrain, "running"):
		_fail("hierarchical baked world did not reach running")
		return
	if terrain.call("get_world_page_count") != 28:
		_fail("hierarchical baked world page count mismatch")
		return

	if not terrain.call("update_viewer", 1, 1, Vector3(8, 8, 8), 1, 1):
		_fail("initial multi-LOD viewer event was rejected")
		return
	if not await _wait_for_counts(terrain, 5, 5):
		_fail("initial multi-LOD resources did not settle")
		return
	var bridge: Node = terrain.get_node_or_null("WT_Render_1_0_0_L1")
	if bridge == null or terrain.get_node_or_null("WT_Render_0_0_0_L0") == null:
		_fail("initial fine/coarse resource identity mismatch")
		return
	var first_bridge_id := bridge.get_instance_id()

	if not terrain.call("update_viewer", 2, 1, Vector3(80, 8, 8), 1, 1):
		_fail("second multi-LOD viewer event was rejected")
		return
	if not await _wait_for_counts(terrain, 10, 10):
		_fail("two-viewer balanced resources did not settle")
		return
	bridge = terrain.get_node_or_null("WT_Render_1_0_0_L1")
	if bridge == null or bridge.get_instance_id() == first_bridge_id or \
			terrain.get_node_or_null("WT_Render_3_0_0_L1") == null:
		_fail("transition-mask change did not replace the coarse bridge")
		return

	if not terrain.call("update_viewer", 1, 2, Vector3(40, 8, 8), 1, 1):
		_fail("moving multi-LOD viewer event was rejected")
		return
	if not await _wait_for_counts(terrain, 13, 13):
		_fail("moving multi-LOD resources did not settle")
		return

	if not terrain.call("remove_viewer", 1, 3):
		_fail("first multi-LOD viewer removal was rejected")
		return
	if not await _wait_for_counts(terrain, 6, 6):
		_fail("single-viewer multi-LOD resources did not settle")
		return
	if not terrain.call("remove_viewer", 2, 2):
		_fail("second multi-LOD viewer removal was rejected")
		return
	if not await _wait_for_counts(terrain, 0, 0):
		_fail("multi-LOD viewer removal did not evict resources")
		return
	if not terrain.call("stop_world") or \
			not await _wait_for_state(terrain, "stopped"):
		_fail("multi-LOD world did not stop cleanly")
		return
	print("PRODUCTION_GODOT_LOD_STREAMING_PASS pages=28 viewers=2 transitions=3")
	terrain.queue_free()
	await process_frame
	quit(0)


func _wait_for_state(terrain: Node, expected: String) -> bool:
	for _frame in range(900):
		if terrain.call("get_world_state_name") == expected:
			await process_frame
			return true
		await process_frame
	return false


func _wait_for_counts(terrain: Node, render_count: int, collision_count: int) -> bool:
	for _frame in range(900):
		if terrain.call("get_rendered_chunk_count") == render_count and \
				terrain.call("get_collision_chunk_count") == collision_count:
			await process_frame
			return true
		await process_frame
	return false


func _fail(message: String) -> void:
	push_error("PRODUCTION_GODOT_LOD_STREAMING_FAIL: " + message)
	quit(1)
