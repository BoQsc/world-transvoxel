extends SceneTree


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	var config: Resource = ClassDB.instantiate("WorldTransvoxelConfig")
	var terrain: Node = ClassDB.instantiate("WorldTransvoxelTerrain")
	if config == null or terrain == null or \
			not ClassDB.class_exists("WorldTransvoxelChunkState"):
		_fail("query classes could not be instantiated")
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
		"start_world", fixture_root + "/transition.wtworld", fixture_root
	) or not await _wait_for_state(terrain, "running"):
		_fail("query fixture did not reach running")
		return

	var absent: RefCounted = terrain.call(
		"query_chunk_state", Vector3i(1, 0, 0), 1
	)
	if absent == null or absent.call("is_present") or \
			absent.call("get_chunk_coordinate") != Vector3i(1, 0, 0) or \
			absent.call("get_lod") != 1 or absent.call("get_generation") != 0:
		_fail("valid absent chunk snapshot mismatch")
		return
	if absent.call("get_render_generation") != 0 or \
			absent.call("get_staged_render_generation") != 0 or \
			absent.call("get_collision_generation") != 0 or \
			absent.call("get_staged_collision_generation") != 0:
		_fail("absent chunk exposed applied resource generations")
		return
	if terrain.call("query_chunk_state", Vector3i.ZERO, -1) != null or \
			terrain.call("query_chunk_state", Vector3i.ZERO, 21) != null:
		_fail("invalid LOD query was accepted")
		return

	if not terrain.call("update_viewer", 1, 1, Vector3(8, 8, 8), 1, 1) or \
			not await _wait_for_counts(terrain, 5, 5):
		_fail("initial query plan did not settle")
		return
	var active_states: Array = terrain.call("query_active_chunk_states")
	var active_metrics: Dictionary = terrain.call("get_runtime_metrics")
	if active_states.size() != int(active_metrics.get("active_chunk_records", -1)) or \
			active_states.size() <= terrain.call("get_rendered_chunk_count") or \
			not _active_states_are_canonical(active_states):
		_fail("active chunk enumeration is incomplete or non-canonical")
		return
	var first_bridge: RefCounted = terrain.call(
		"query_chunk_state", Vector3i(1, 0, 0), 1
	)
	if not _is_ready_snapshot(first_bridge) or \
			first_bridge.call("get_generation") <= 0 or \
			not _has_matching_applied_generations(first_bridge):
		_fail("coarse bridge readiness snapshot mismatch")
		return
	var empty_surface: RefCounted = terrain.call(
		"query_chunk_state", Vector3i(0, 1, 0), 0
	)
	if not _is_ready_snapshot(empty_surface) or \
			terrain.get_node_or_null("WT_Render_0_1_0_L0") != null:
		_fail("active empty-surface snapshot mismatch")
		return
	var first_generation: int = first_bridge.call("get_generation")

	if not terrain.call("update_viewer", 2, 1, Vector3(80, 8, 8), 1, 1) or \
			not await _wait_for_counts(terrain, 10, 10):
		_fail("second query viewer plan did not settle")
		return
	active_states = terrain.call("query_active_chunk_states")
	active_metrics = terrain.call("get_runtime_metrics")
	if active_states.size() != int(active_metrics.get("active_chunk_records", -1)) or \
			active_states.size() <= terrain.call("get_rendered_chunk_count") or \
			not _active_states_are_canonical(active_states):
		_fail("active chunk enumeration did not follow viewer ownership")
		return
	var second_bridge: RefCounted = terrain.call(
		"query_chunk_state", Vector3i(1, 0, 0), 1
	)
	var second_generation := (
		int(second_bridge.call("get_generation")) if second_bridge != null else 0
	)
	if not _is_ready_snapshot(second_bridge) or \
			second_generation <= first_generation or \
			not _has_matching_applied_generations(second_bridge) or \
			first_bridge.call("get_generation") != first_generation:
		_fail(
			"exact-mask replacement snapshot mismatch first=%d current=%d render=%d staged=%d collision=%d staged_collision=%d ready=%s" %
			[
				first_generation,
				second_bridge.call("get_generation"),
				second_bridge.call("get_render_generation"),
				second_bridge.call("get_staged_render_generation"),
				second_bridge.call("get_collision_generation"),
				second_bridge.call("get_staged_collision_generation"),
				str(_is_ready_snapshot(second_bridge)),
			]
		)
		return

	if not terrain.call("remove_viewer", 1, 2) or \
			not terrain.call("remove_viewer", 2, 2) or \
			not await _wait_for_counts(terrain, 0, 0):
		_fail("query viewer removal did not settle")
		return
	var evicted: RefCounted = terrain.call(
		"query_chunk_state", Vector3i(1, 0, 0), 1
	)
	if evicted == null or evicted.call("is_present"):
		_fail("evicted chunk remained query-visible")
		return

	if not terrain.call("stop_world") or \
			not await _wait_for_state(terrain, "stopped"):
		_fail("query fixture did not stop cleanly")
		return
	print(
		"PRODUCTION_GODOT_CHUNK_QUERY_PASS snapshots=5 enumeration=1 exact_mask_replacement=1"
	)
	terrain.queue_free()
	await process_frame
	quit(0)


func _is_ready_snapshot(snapshot: RefCounted) -> bool:
	return snapshot != null and snapshot.call("is_present") and \
		snapshot.call("is_visual_ready") and \
		snapshot.call("is_collision_required") and \
		snapshot.call("is_collision_ready") and \
		snapshot.call("is_fully_ready")


func _has_matching_applied_generations(snapshot: RefCounted) -> bool:
	var generation: int = snapshot.call("get_generation")
	return snapshot.call("get_render_generation") == generation and \
		snapshot.call("get_staged_render_generation") == 0 and \
		snapshot.call("get_collision_generation") == generation and \
		snapshot.call("get_staged_collision_generation") == 0


func _active_states_are_canonical(states: Array) -> bool:
	var previous_coordinate := Vector3i.ZERO
	var previous_lod := -1
	for state_value in states:
		var state := state_value as RefCounted
		if state == null or not bool(state.call("is_present")):
			return false
		var coordinate: Vector3i = state.call("get_chunk_coordinate")
		var lod := int(state.call("get_lod"))
		if previous_lod >= 0 and not _key_less(
			previous_coordinate, previous_lod, coordinate, lod
		):
			return false
		previous_coordinate = coordinate
		previous_lod = lod
	return true


func _key_less(
	left: Vector3i,
	left_lod: int,
	right: Vector3i,
	right_lod: int
) -> bool:
	if left_lod != right_lod:
		return left_lod < right_lod
	if left.z != right.z:
		return left.z < right.z
	if left.y != right.y:
		return left.y < right.y
	return left.x < right.x


func _wait_for_state(terrain: Node, expected: String) -> bool:
	for _frame in range(900):
		if terrain.call("get_world_state_name") == expected:
			await process_frame
			return true
		await process_frame
	return false


func _wait_for_counts(terrain: Node, render_count: int, collision_count: int) -> bool:
	for _frame in range(900):
		var metrics: Dictionary = terrain.call("get_runtime_metrics")
		if terrain.call("get_rendered_chunk_count") == render_count and \
				terrain.call("get_collision_chunk_count") == collision_count and \
				int(metrics.get("queued_render", 0)) == 0 and \
				int(metrics.get("queued_collision", 0)) == 0 and \
				int(metrics.get("fully_ready_chunk_records", -1)) == \
				int(metrics.get("active_chunk_records", 0)) and \
				int(metrics.get("pending_chunk_retirements", 0)) == 0:
			await process_frame
			return true
		await process_frame
	return false


func _fail(message: String) -> void:
	push_error("PRODUCTION_GODOT_CHUNK_QUERY_FAIL: " + message)
	quit(1)
