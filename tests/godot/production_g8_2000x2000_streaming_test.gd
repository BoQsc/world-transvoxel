extends SceneTree


const ACTIVE_CHUNK_BUDGET := 256
const EXPECTED_PAGE_COUNT := 93
const PATH_SAMPLES := [
	{
		"label": "origin",
		"position": Vector3(8, 8, 8),
		"expected_chunks": 9,
		"center_chunk": Vector3i(0, 0, 0),
	},
	{
		"label": "near_center",
		"position": Vector3(496, 8, 496),
		"expected_chunks": 25,
		"center_chunk": Vector3i(31, 0, 31),
	},
	{
		"label": "center",
		"position": Vector3(1000, 8, 1000),
		"expected_chunks": 25,
		"center_chunk": Vector3i(62, 0, 62),
	},
	{
		"label": "edge",
		"position": Vector3(1504, 8, 496),
		"expected_chunks": 25,
		"center_chunk": Vector3i(94, 0, 31),
	},
	{
		"label": "far_corner",
		"position": Vector3(1991, 8, 1991),
		"expected_chunks": 9,
		"center_chunk": Vector3i(124, 0, 124),
	},
]


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	var config: Resource = ClassDB.instantiate("WorldTransvoxelConfig")
	var terrain: Node = ClassDB.instantiate("WorldTransvoxelTerrain")
	if config == null or terrain == null:
		_fail("g8 runtime classes could not be instantiated")
		return
	config.set("active_chunk_capacity", ACTIVE_CHUNK_BUDGET)
	config.set("viewer_capacity", 1)
	config.set("demand_capacity_per_viewer", 125)
	config.set("storage_request_capacity", ACTIVE_CHUNK_BUDGET)
	config.set("storage_completion_capacity", ACTIVE_CHUNK_BUDGET)
	config.set("encoded_page_entry_capacity", ACTIVE_CHUNK_BUDGET)
	config.set("decoded_page_entry_capacity", ACTIVE_CHUNK_BUDGET)
	config.set("mesh_entry_capacity", ACTIVE_CHUNK_BUDGET)
	config.set("render_entry_capacity", ACTIVE_CHUNK_BUDGET)
	config.set("collision_entry_capacity", ACTIVE_CHUNK_BUDGET)
	config.set("render_apply_budget", 128)
	config.set("collision_apply_budget", 128)
	config.set("render_transition_frames", 0)
	if not config.call("is_valid"):
		_fail("g8 runtime configuration invalid: %s" % config.call("get_validation_error"))
		return

	root.add_child(terrain)
	terrain.set("configuration", config)

	const fixture_root := "res://build/production-lifecycle-fixture"
	if not terrain.call(
		"start_world",
		fixture_root + "/g8_2000x2000_sparse.wtworld",
		fixture_root
	):
		_fail("g8 sparse baked world startup was rejected")
		return
	if not await _wait_for_state(terrain, "running"):
		_fail("g8 sparse baked world did not reach running")
		return
	if terrain.call("get_world_page_count") != EXPECTED_PAGE_COUNT:
		_fail(
			"g8 page count mismatch: %s" %
			terrain.call("get_world_page_count")
		)
		return

	var max_render_resources := 0
	var max_collision_resources := 0
	var revision := 1
	for sample in PATH_SAMPLES:
		if not terrain.call(
			"update_viewer",
			1,
			revision,
			sample["position"],
			2,
			0
		):
			_fail("g8 viewer event rejected at %s" % sample["label"])
			return
		revision += 1
		if not await _wait_for_window(
			terrain,
			int(sample["expected_chunks"]),
			sample["label"],
			revision - 1,
			sample["center_chunk"]
		):
			return
		var metrics: Dictionary = terrain.call("get_runtime_metrics")
		max_render_resources = max(
			max_render_resources,
			int(metrics.get("render_resources", 0))
		)
		max_collision_resources = max(
			max_collision_resources,
			int(metrics.get("collision_resources", 0))
		)

	if not terrain.call("remove_viewer", 1, revision):
		_fail("g8 viewer removal was rejected")
		return
	if not await _wait_for_window(
		terrain,
		0,
		"viewer_removed",
		PATH_SAMPLES.size(),
		null
	):
		return
	if not terrain.call("stop_world") or \
			not await _wait_for_state(terrain, "stopped"):
		_fail("g8 sparse world did not stop cleanly")
		return
	print("PRODUCTION_GODOT_G8_2000X2000_STREAMING_PASS pages=%d samples=%d max_render_resources=%d max_collision_resources=%d active_budget=%d" % [
		EXPECTED_PAGE_COUNT,
		PATH_SAMPLES.size(),
		max_render_resources,
		max_collision_resources,
		ACTIVE_CHUNK_BUDGET,
	])
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


func _wait_for_window(
	terrain: Node,
	expected_chunks: int,
	label: String,
	minimum_viewer_updates: int,
	center_chunk: Variant
) -> bool:
	for _frame in range(1500):
		var metrics: Dictionary = terrain.call("get_runtime_metrics")
		var active_records := int(metrics.get("active_chunk_records", 0))
		var render_resources := int(metrics.get("render_resources", 0))
		var collision_resources := int(metrics.get("collision_resources", 0))
		var center_ready := true
		if center_chunk != null:
			center_ready = _is_ready_snapshot(
				terrain.call("query_chunk_state", center_chunk, 0)
			)
		if active_records == expected_chunks and \
				render_resources == expected_chunks and \
				collision_resources == expected_chunks and \
				int(metrics.get("viewer_updates", 0)) >= minimum_viewer_updates and \
				center_ready and \
				int(metrics.get("queued_render", 0)) == 0 and \
				int(metrics.get("queued_collision", 0)) == 0 and \
				int(metrics.get("fully_ready_chunk_records", -1)) == \
				active_records and \
				int(metrics.get("pending_chunk_retirements", 0)) == 0 and \
				int(metrics.get("render_fading_resources", 0)) == 0:
			if active_records <= ACTIVE_CHUNK_BUDGET:
				return true
		await process_frame
	var final_metrics: Dictionary = terrain.call("get_runtime_metrics")
	_fail(
		"g8 window did not settle at %s expected=%s render=%s collision=%s metrics=%s" %
		[
			label,
			expected_chunks,
			terrain.call("get_rendered_chunk_count"),
			terrain.call("get_collision_chunk_count"),
			final_metrics,
		]
	)
	return false


func _is_ready_snapshot(snapshot: RefCounted) -> bool:
	return snapshot != null and snapshot.call("is_present") and \
			snapshot.call("is_visual_ready") and \
			snapshot.call("is_collision_required") and \
			snapshot.call("is_collision_ready") and \
			snapshot.call("is_fully_ready")


func _fail(message: String) -> void:
	push_error("PRODUCTION_GODOT_G8_2000X2000_STREAMING_FAIL: " + message)
	quit(1)
