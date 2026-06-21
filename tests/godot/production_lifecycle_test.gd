extends SceneTree


var observed_states: Array[String] = []
var observed_failures: Array[String] = []


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	var config: Resource = ClassDB.instantiate("WorldTransvoxelConfig")
	var terrain: Node = ClassDB.instantiate("WorldTransvoxelTerrain")
	if config == null or terrain == null:
		_fail("lifecycle classes could not be instantiated")
		return
	root.add_child(terrain)
	terrain.set("configuration", config)
	terrain.connect("world_state_changed", _on_world_state_changed)
	terrain.connect("world_failed", _on_world_failed)

	const fixture_root := "res://build/production-lifecycle-fixture"
	const world_path := fixture_root + "/world.wtworld"
	if not terrain.call("start_world", world_path, fixture_root):
		_fail("valid asynchronous startup was rejected")
		return
	if not await _wait_for_state(terrain, "running"):
		_fail("valid manifest did not reach running state")
		return
	if not terrain.call("is_world_running") or \
			terrain.call("get_world_state") != 2 or \
			terrain.call("get_world_source_revision") != 7001 or \
			terrain.call("get_world_revision") != 12 or \
			terrain.call("get_world_page_count") != 0:
		_fail("running manifest metadata mismatch")
		return
	if not observed_states.has("starting") or not observed_states.has("running"):
		_fail("startup state signals were incomplete")
		return

	var replacement: Resource = ClassDB.instantiate("WorldTransvoxelConfig")
	replacement.set("active_chunk_capacity", 32)
	terrain.set("configuration", replacement)
	if terrain.get("configuration") != config:
		_fail("running world configuration was replaced")
		return
	if terrain.call("start_world", world_path, fixture_root) or \
			terrain.call("get_world_error") != \
			"world lifecycle state does not allow startup":
		_fail("double startup was not rejected")
		return

	if not terrain.call("stop_world"):
		_fail("asynchronous stop request was rejected")
		return
	if not await _wait_for_state(terrain, "stopped"):
		_fail("world did not finish stopping")
		return
	if terrain.call("get_world_source_revision") != 0 or \
			terrain.call("get_world_revision") != 0 or \
			terrain.call("get_world_page_count") != 0 or \
			not observed_states.has("stopping") or \
			not observed_states.has("stopped"):
		_fail("stopped world retained state or missed signals")
		return

	if not terrain.call(
		"start_world",
		fixture_root + "/missing.wtworld",
		fixture_root
	):
		_fail("missing manifest was rejected synchronously")
		return
	if not await _wait_for_state(terrain, "failed"):
		_fail("missing manifest did not reach failed state")
		return
	if terrain.call("get_world_state") != 4 or \
			terrain.call("get_world_error") != \
			"manifest or object-root path is invalid" or \
			observed_failures.size() != 1:
		_fail("asynchronous manifest failure mismatch")
		return
	if not terrain.call("stop_world") or \
			not await _wait_for_state(terrain, "stopped"):
		_fail("failed lifecycle did not reset to stopped")
		return

	if not terrain.call("start_world", world_path, fixture_root) or \
			not await _wait_for_state(terrain, "running"):
		_fail("world did not restart after asynchronous failure")
		return
	terrain.queue_free()
	await process_frame
	print("PRODUCTION_GODOT_LIFECYCLE_PASS states=5 restart=1")
	quit(0)


func _wait_for_state(terrain: Node, expected: String) -> bool:
	for _frame in range(300):
		if terrain.call("get_world_state_name") == expected:
			# State changes originate on the native control thread. Give the
			# terrain's process callback one frame to publish the matching signal.
			await process_frame
			return true
		await process_frame
	return false


func _on_world_state_changed(_state: int, state_name: String) -> void:
	observed_states.push_back(state_name)


func _on_world_failed(error: String) -> void:
	observed_failures.push_back(error)


func _fail(message: String) -> void:
	push_error("PRODUCTION_GODOT_LIFECYCLE_FAIL: " + message)
	quit(1)
