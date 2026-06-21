extends SceneTree


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	var config: Resource = ClassDB.instantiate("WorldTransvoxelConfig")
	var terrain: Node = ClassDB.instantiate("WorldTransvoxelTerrain")
	if config == null or terrain == null:
		_fail("streaming classes could not be instantiated")
		return
	config.set("active_chunk_capacity", 8)
	config.set("viewer_capacity", 2)
	config.set("demand_capacity_per_viewer", 125)
	config.set("storage_request_capacity", 16)
	config.set("storage_completion_capacity", 16)
	config.set("encoded_page_entry_capacity", 8)
	config.set("decoded_page_entry_capacity", 8)
	config.set("mesh_entry_capacity", 8)
	config.set("render_entry_capacity", 8)
	config.set("collision_entry_capacity", 8)
	root.add_child(terrain)
	terrain.set("configuration", config)

	const fixture_root := "res://build/production-lifecycle-fixture"
	if not terrain.call(
		"start_world",
		fixture_root + "/streaming.wtworld",
		fixture_root
	):
		_fail("non-empty baked world startup was rejected")
		return
	if not await _wait_for_state(terrain, "running"):
		_fail("non-empty baked world did not reach running")
		return
	if terrain.call("get_world_page_count") != 4:
		_fail("non-empty baked world page count mismatch")
		return

	if not terrain.call("update_viewer", 1, 1, Vector3(8, 8, 8), 0):
		_fail("initial viewer event was rejected")
		return
	if not await _wait_for_counts(terrain, 1, 1):
		_fail("initial baked page did not render and collide")
		return
	if terrain.get_node_or_null("WT_Render_0_0_0_L0") == null:
		_fail("initial rendered chunk identity mismatch")
		return

	if not terrain.call("update_viewer", 1, 2, Vector3(24, 8, 8), 0):
		_fail("moving viewer event was rejected")
		return
	if not await _wait_for_chunk(terrain, "WT_Render_1_0_0_L0"):
		_fail("moving viewer did not replace the rendered chunk")
		return

	if not terrain.call("update_viewer", 2, 1, Vector3(8, -24, 8), 2):
		_fail("underground second viewer event was rejected")
		return
	if not await _wait_for_counts(terrain, 4, 4):
		_fail(
			"multi-viewer underground demand did not stream all pages " +
			"render=%s collision=%s state=%s error=%s" % [
				terrain.call("get_rendered_chunk_count"),
				terrain.call("get_collision_chunk_count"),
				terrain.call("get_world_state_name"),
				terrain.call("get_world_error"),
			]
		)
		return
	if not terrain.call("update_viewer", 2, 2, Vector3(8, 40, 8), 2):
		_fail("vertical viewer event was rejected")
		return
	await process_frame

	if not terrain.call("remove_viewer", 1, 3) or \
			not terrain.call("remove_viewer", 2, 3):
		_fail("viewer removal was rejected")
		return
	if not await _wait_for_counts(terrain, 0, 0):
		_fail("viewer removal did not evict Godot resources")
		return
	if not terrain.call("stop_world") or \
			not await _wait_for_state(terrain, "stopped"):
		_fail("streaming world did not stop cleanly")
		return
	print("PRODUCTION_GODOT_STREAMING_PASS pages=4 viewers=2 movement=4")
	terrain.queue_free()
	await process_frame
	quit(0)


func _wait_for_state(terrain: Node, expected: String) -> bool:
	for _frame in range(600):
		if terrain.call("get_world_state_name") == expected:
			await process_frame
			return true
		await process_frame
	return false


func _wait_for_counts(terrain: Node, render_count: int, collision_count: int) -> bool:
	for _frame in range(600):
		if terrain.call("get_rendered_chunk_count") == render_count and \
				terrain.call("get_collision_chunk_count") == collision_count:
			return true
		await process_frame
	return false


func _wait_for_chunk(terrain: Node, chunk_name: String) -> bool:
	for _frame in range(600):
		if terrain.get_node_or_null(chunk_name) != null:
			return true
		await process_frame
	return false


func _fail(message: String) -> void:
	push_error("PRODUCTION_GODOT_STREAMING_FAIL: " + message)
	quit(1)
