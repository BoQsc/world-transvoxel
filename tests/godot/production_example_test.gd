extends SceneTree


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	const scene_path := "res://world_transvoxel/example_world.tscn"
	if ProjectSettings.get_setting("application/run/main_scene") != scene_path:
		_fail("root project does not select the example scene")
		return
	var packed: PackedScene = load(scene_path)
	if packed == null:
		_fail("example scene could not be loaded")
		return
	var example: Node3D = packed.instantiate()
	if example == null:
		_fail("example scene could not be instantiated")
		return
	root.add_child(example)
	var terrain: Node = example.get_node_or_null("Terrain")
	var viewer: Node3D = example.get_node_or_null("Viewer")
	if terrain == null or viewer == null:
		_fail("example scene ownership is incomplete")
		return
	if not await _wait_for_state(terrain, "running"):
		_fail("example world did not reach running")
		return
	if terrain.call("get_world_page_count") != 28:
		_fail("example world page count mismatch")
		return
	if not await _wait_for_counts(terrain, 5, 5):
		_fail("initial example LOD plan did not settle")
		return
	if terrain.get_node_or_null("WT_Render_0_0_0_L0") == null or \
			terrain.get_node_or_null("WT_Render_1_0_0_L1") == null:
		_fail("initial example fine/coarse resources are missing")
		return

	viewer.position = Vector3(80, 8, 8)
	if not await _wait_for_counts(terrain, 6, 6):
		_fail("example transform event did not stream the second region")
		return
	if terrain.get_node_or_null("WT_Render_4_0_0_L0") == null or \
			terrain.get_node_or_null("WT_Render_3_0_0_L1") == null:
		_fail("moved example resources have incorrect identities")
		return

	viewer.position = Vector3(40, 8, 8)
	if not await _wait_for_counts(terrain, 9, 9):
		_fail("example transform event did not settle balanced refinement")
		return
	if terrain.get_node_or_null("WT_Render_2_0_0_L0") == null or \
			terrain.get_node_or_null("WT_Render_2_0_0_L1") == null:
		_fail("balanced example resources have incorrect identities")
		return

	if not example.call("stop_example") or \
			not await _wait_for_state(terrain, "stopped"):
		_fail("example world did not stop cleanly")
		return
	if terrain.call("get_rendered_chunk_count") != 0 or \
			terrain.call("get_collision_chunk_count") != 0:
		_fail("example shutdown retained resources")
		return
	print("PRODUCTION_GODOT_EXAMPLE_PASS pages=28 moves=2 shutdown=clean")
	example.queue_free()
	await process_frame
	quit(0)


func _wait_for_state(terrain: Node, expected: String) -> bool:
	for _frame in range(900):
		if terrain.call("get_world_state_name") == expected:
			await process_frame
			return true
		if terrain.call("get_world_state_name") == "failed":
			return false
		await process_frame
	return false


func _wait_for_counts(terrain: Node, render_count: int, collision_count: int) -> bool:
	for _frame in range(900):
		var metrics: Dictionary = terrain.call("get_runtime_metrics")
		if terrain.call("get_rendered_chunk_count") == render_count and \
				terrain.call("get_collision_chunk_count") == collision_count and \
				int(metrics.get("queued_render", 0)) == 0 and \
				int(metrics.get("queued_collision", 0)) == 0 and \
				int(metrics.get("pending_chunk_retirements", 0)) == 0:
			await process_frame
			return true
		if terrain.call("get_world_state_name") == "failed":
			return false
		await process_frame
	return false


func _fail(message: String) -> void:
	push_error("PRODUCTION_GODOT_EXAMPLE_FAIL: " + message)
	quit(1)
