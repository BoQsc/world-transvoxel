extends SceneTree


var committed_revisions: Array[int] = []
var edit_failures: Array[String] = []


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	var config: Resource = ClassDB.instantiate("WorldTransvoxelConfig")
	var terrain: Node = ClassDB.instantiate("WorldTransvoxelTerrain")
	if config == null or terrain == null or \
			not ClassDB.class_exists("WorldTransvoxelEditTransaction"):
		_fail("edit classes could not be instantiated")
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
	terrain.connect("edit_committed", _on_edit_committed)
	terrain.connect("edit_failed", _on_edit_failed)

	const fixture_root := "res://build/production-lifecycle-fixture"
	const world_path := fixture_root + "/streaming.wtworld"
	const journal_path := fixture_root + "/world.wtedit"
	var journal_absolute := ProjectSettings.globalize_path(journal_path)
	if FileAccess.file_exists(journal_path):
		DirAccess.remove_absolute(journal_absolute)

	if not terrain.call("start_world", world_path, fixture_root) or \
			not await _wait_for_state(terrain, "running"):
		_fail("editable world did not reach running")
		return
	if terrain.call("get_world_revision") != 12 or \
			not terrain.call("update_viewer", 1, 1, Vector3(8, 8, 8), 0) or \
			not await _wait_for_ready_chunk(terrain, 0, true):
		_fail("initial editable chunk did not settle")
		return
	var initial: RefCounted = terrain.call(
		"query_chunk_state", Vector3i.ZERO, 0
	)
	var initial_generation: int = initial.call("get_generation")
	var render_node: MeshInstance3D = terrain.get_node_or_null(
		"WT_Render_0_0_0_L0"
	)
	if render_node == null or render_node.mesh == null or \
			render_node.mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX].size() == 0:
		_fail("initial editable mesh was empty")
		return

	var invalid: RefCounted = terrain.call("begin_edit_transaction")
	if invalid == null or invalid.call(
		"set_density_sphere", Vector3.ZERO, 0.0, 1.0
	) or invalid.call("get_error") == "ok":
		_fail("invalid edit parameters were accepted")
		return

	var first: RefCounted = terrain.call("begin_edit_transaction", 41)
	var stale: RefCounted = terrain.call("begin_edit_transaction", 42)
	if first == null or stale == null or \
			not first.call(
				"set_density_box",
				Vector3(-2, -2, -2),
				Vector3(18, 18, 18),
				10.0
			) or \
			not stale.call(
				"add_density_sphere", Vector3(8, 8, 8), 2.0, -1.0
			):
		_fail("valid edit transaction construction failed")
		return
	if not terrain.call("commit_edit_transaction", first) or \
			not first.call("is_submitted") or \
			not await _wait_for_commit(terrain, 13):
		_fail("durable edit commit did not publish")
		return
	if not await _wait_for_ready_chunk(
		terrain, initial_generation, false
	):
		_fail("edited chunk generation did not settle as empty")
		return
	if not FileAccess.file_exists(journal_path):
		_fail("durable edit journal was not created")
		return
	var journal := FileAccess.open(journal_path, FileAccess.READ)
	if journal == null or journal.get_length() <= 0:
		_fail("durable edit journal has no committed bytes")
		return
	journal.close()

	if not terrain.call("commit_edit_transaction", stale) or \
			not await _wait_for_edit_failure():
		_fail("stale edit did not publish rejection")
		return
	if edit_failures.back() != "edit transaction world revision is stale" or \
			terrain.call("get_world_revision") != 13:
		_fail("stale edit rejection state mismatch")
		return

	if not terrain.call("stop_world") or \
			not await _wait_for_state(terrain, "stopped") or \
			not terrain.call("start_world", world_path, fixture_root) or \
			not await _wait_for_state(terrain, "running"):
		_fail("edited world restart failed")
		return
	if terrain.call("get_world_revision") != 13 or \
			not terrain.call("update_viewer", 1, 1, Vector3(8, 8, 8), 0) or \
			not await _wait_for_ready_chunk(terrain, 0, false):
		_fail("restart did not replay committed edit")
		return
	if terrain.get_node_or_null("WT_Render_0_0_0_L0") != null:
		_fail("restart replay restored the pre-edit surface")
		return

	if not terrain.call("stop_world") or \
			not await _wait_for_state(terrain, "stopped"):
		_fail("edited world did not stop cleanly")
		return
	DirAccess.remove_absolute(journal_absolute)
	print(
		"PRODUCTION_GODOT_EDIT_JOURNAL_PASS commits=1 stale=1 " +
		"replacement=1 restart_replay=1"
	)
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


func _wait_for_commit(terrain: Node, revision: int) -> bool:
	for _frame in range(900):
		if committed_revisions.has(revision) and \
				terrain.call("get_world_revision") == revision:
			return true
		await process_frame
	return false


func _wait_for_edit_failure() -> bool:
	for _frame in range(900):
		if not edit_failures.is_empty():
			return true
		await process_frame
	return false


func _wait_for_ready_chunk(
	terrain: Node,
	previous_generation: int,
	expect_resources: bool
) -> bool:
	for _frame in range(900):
		var snapshot: RefCounted = terrain.call(
			"query_chunk_state", Vector3i.ZERO, 0
		)
		if snapshot != null and snapshot.call("is_fully_ready") and \
				snapshot.call("get_generation") > previous_generation:
			var expected_count := 1 if expect_resources else 0
			if terrain.call("get_rendered_chunk_count") == expected_count and \
					terrain.call("get_collision_chunk_count") == expected_count:
				return true
		await process_frame
	return false


func _on_edit_committed(world_revision: int) -> void:
	committed_revisions.push_back(world_revision)


func _on_edit_failed(error: String) -> void:
	edit_failures.push_back(error)


func _fail(message: String) -> void:
	push_error("PRODUCTION_GODOT_EDIT_JOURNAL_FAIL: " + message)
	quit(1)
