extends SceneTree


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	var terrain := WorldTransvoxelTerrain.new()
	root.add_child(terrain)
	terrain.set_render_apply_budget(0)
	terrain.set_collision_apply_budget(0)

	if not terrain.call("_m3_test_submit_generation", 1, true):
		return _fail("generation 1 submission failed")
	if not terrain.call("_m3_test_submit_generation", 2, true):
		return _fail("generation 2 supersession failed")
	await process_frame
	if terrain.get_queued_render_count() != 2 or terrain.get_queued_collision_count() != 2:
		return _fail("zero application budget performed queue work")
	terrain.set_render_apply_budget(1)
	terrain.set_collision_apply_budget(1)
	await process_frame
	if terrain.get_render_resource_count() != 0 or \
			terrain.get_collision_resource_count() != 0:
		return _fail("stale resources reached Godot")
	if terrain.get_queued_render_count() != 1 or terrain.get_queued_collision_count() != 1:
		return _fail("application budget was not enforced")
	await process_frame
	if terrain.get_render_resource_count() != 1 or \
			terrain.get_collision_resource_count() != 1:
		return _fail("current resources were not created")
	if not terrain.call("_m3_test_fully_ready"):
		return _fail("chunk did not become fully ready")
	if terrain.call("_m3_test_render_generation") != 2 or \
			terrain.call("_m3_test_collision_generation") != 2:
		return _fail("wrong generation reached Godot resources")
	if terrain.call("_m3_test_stale_render_count") != 1 or \
			terrain.call("_m3_test_stale_collision_count") != 1:
		return _fail("stale metrics mismatch")
	if terrain.get_render_latency_frames_maximum() <= 0 or \
			terrain.get_collision_latency_frames_maximum() <= 0:
		return _fail("application latency telemetry was not recorded")

	for cycle in range(16):
		if not terrain.call("_m3_test_set_collision_distance", 129.0):
			return _fail("collision deactivation failed")
		if terrain.get_collision_resource_count() != 0 or \
				not terrain.call("_m3_test_fully_ready"):
			return _fail("far-viewer collision state mismatch")
		await process_frame
		if not terrain.call("_m3_test_set_collision_distance", 95.0):
			return _fail("collision activation failed")
		if terrain.call("_m3_test_fully_ready"):
			return _fail("collision became ready before application")
		await process_frame
		if terrain.get_collision_resource_count() != 1 or \
				not terrain.call("_m3_test_fully_ready"):
			return _fail("near-viewer collision state mismatch")
		if terrain.get_render_resource_count() > 1 or \
				terrain.get_collision_resource_count() > 1:
			return _fail("viewer movement created unbounded resources")

	if not terrain.call("_m3_test_submit_generation", 3, true):
		return _fail("generation 3 replacement failed")
	await process_frame
	if terrain.call("_m3_test_render_generation") != 3 or \
			terrain.call("_m3_test_collision_generation") != 3:
		return _fail("resource replacement retained an old generation")
	if terrain.get_render_resource_count() != 1 or \
			terrain.get_collision_resource_count() != 1:
		return _fail("resource replacement duplicated chunk nodes")

	if not terrain.call("_m3_test_submit_generation", 4, true):
		return _fail("in-flight teardown generation failed")
	terrain.call("_m3_test_forget_chunk")
	if terrain.get_render_resource_count() != 0 or \
			terrain.get_collision_resource_count() != 0:
		return _fail("resource teardown retained ownership")
	await process_frame
	if terrain.get_queued_render_count() != 0 or terrain.get_queued_collision_count() != 0:
		return _fail("in-flight teardown retained queued work")
	if terrain.call("_m3_test_stale_render_count") != 2 or \
			terrain.call("_m3_test_stale_collision_count") != 2:
		return _fail("in-flight teardown did not reject stale results")

	terrain.set_render_apply_budget(0)
	terrain.set_collision_apply_budget(0)
	if not terrain.call("_m3_test_submit_generation", 5, true):
		return _fail("shutdown generation failed")
	terrain.queue_free()
	await process_frame
	print("M3_GODOT_INTEGRATION_PASS movement_cycles=16")
	quit(0)


func _fail(message: String) -> void:
	push_error("M3_GODOT_INTEGRATION_FAIL: " + message)
	quit(1)
