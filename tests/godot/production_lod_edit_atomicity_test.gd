extends SceneTree


var visual_collision_mismatch_frames := 0
var visible_cross_lod_overlap_frames := 0
var collision_overlap_frames := 0
var maximum_staged_collision_resources := 0
var first_mismatch := ""


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	var config: Resource = ClassDB.instantiate("WorldTransvoxelConfig")
	var terrain: Node = ClassDB.instantiate("WorldTransvoxelTerrain")
	if config == null or terrain == null:
		_fail("LOD edit atomicity classes could not be instantiated")
		return
	config.set("active_chunk_capacity", 40)
	config.set("viewer_capacity", 1)
	config.set("demand_capacity_per_viewer", 125)
	config.set("storage_request_capacity", 64)
	config.set("storage_completion_capacity", 64)
	config.set("encoded_page_entry_capacity", 40)
	config.set("decoded_page_entry_capacity", 40)
	config.set("mesh_entry_capacity", 40)
	config.set("render_entry_capacity", 40)
	config.set("collision_entry_capacity", 40)
	config.set("render_transition_frames", 0)
	root.add_child(terrain)
	terrain.set("configuration", config)

	const fixture_root := "res://build/production-lifecycle-fixture"
	const journal_path := fixture_root + "/world.wtedit"
	var journal_absolute := ProjectSettings.globalize_path(journal_path)
	if FileAccess.file_exists(journal_path):
		DirAccess.remove_absolute(journal_absolute)

	if not terrain.call(
		"start_world",
		fixture_root + "/transition.wtworld",
		fixture_root
	) or not await _wait_for_state(terrain, "running"):
		_fail("LOD edit atomicity fixture did not reach running")
		return
	if terrain.call("get_world_revision") != 13 or not terrain.call(
		"update_viewer", 1, 1, Vector3(8, 8, 8), 1, 1
	) or not await _wait_for_settled(terrain):
		_fail("initial LOD edit atomicity region did not settle")
		return
	visual_collision_mismatch_frames = 0
	visible_cross_lod_overlap_frames = 0
	collision_overlap_frames = 0
	maximum_staged_collision_resources = 0
	first_mismatch = ""

	var render_budget := int(terrain.call("get_render_apply_budget"))
	var collision_budget := int(terrain.call("get_collision_apply_budget"))
	terrain.call("set_render_apply_budget", 0)
	terrain.call("set_collision_apply_budget", 0)
	if not terrain.call(
		"update_viewer", 1, 2, Vector3(40, 8, 8), 1, 1
	) or not await _wait_for_retained_lod_region(terrain):
		_fail("LOD refinement did not retain its coarse coverage")
		return

	var edit: RefCounted = terrain.call("begin_edit_transaction", 51)
	if (
		edit == null or
		not edit.call(
			"add_density_sphere", Vector3(40, 8, 8), 2.0, -1.0
		) or
		not terrain.call("commit_edit_transaction", edit) or
		not await _wait_for_revision(terrain, 14)
	):
		_fail("edit inside the refining LOD region did not commit")
		return

	terrain.call("set_render_apply_budget", render_budget)
	terrain.call("set_collision_apply_budget", collision_budget)
	if not await _wait_for_settled(terrain):
		_fail("edited LOD region did not settle")
		return

	var edited: RefCounted = terrain.call(
		"query_chunk_state", Vector3i(2, 0, 0), 0
	)
	if (
		edited == null or
		not edited.call("is_present") or
		not edited.call("is_fully_ready") or
		int(edited.call("get_generation")) <= 0 or
		int(edited.call("get_render_generation")) !=
			int(edited.call("get_generation")) or
		int(edited.call("get_collision_generation")) !=
			int(edited.call("get_generation")) or
		int(edited.call("get_staged_render_generation")) != 0 or
		int(edited.call("get_staged_collision_generation")) != 0
	):
		_fail("edited fine chunk did not publish one matched generation")
		return
	if (
		visual_collision_mismatch_frames != 0 or
		visible_cross_lod_overlap_frames != 0 or
		collision_overlap_frames != 0
	):
		_fail(
			(
				"LOD edit exposed mixed ownership: visual_collision=%d " +
				"visible_cross_lod=%d collision_overlap=%d first=%s"
			) % [
				visual_collision_mismatch_frames,
				visible_cross_lod_overlap_frames,
				collision_overlap_frames,
				first_mismatch,
			]
		)
		return
	if maximum_staged_collision_resources <= 0:
		_fail("LOD edit atomicity route did not exercise collision staging")
		return

	if (
		not terrain.call("stop_world") or
		not await _wait_for_state(terrain, "stopped")
	):
		_fail("LOD edit atomicity fixture did not stop cleanly")
		return
	if FileAccess.file_exists(journal_path):
		DirAccess.remove_absolute(journal_absolute)
	print(
		"PRODUCTION_GODOT_LOD_EDIT_ATOMICITY_PASS " +
		"revision=14 mixed_ownership_frames=0 staged_collision=%d" %
		maximum_staged_collision_resources
	)
	terrain.queue_free()
	await process_frame
	quit(0)


func _wait_for_state(terrain: Node, expected: String) -> bool:
	for _frame in range(900):
		_audit_frame(terrain)
		if terrain.call("get_world_state_name") == expected:
			await process_frame
			return true
		await process_frame
	return false


func _wait_for_revision(terrain: Node, expected: int) -> bool:
	for _frame in range(1800):
		_audit_frame(terrain)
		if int(terrain.call("get_world_revision")) >= expected:
			return true
		await create_timer(0.001).timeout
	return false


func _wait_for_retained_lod_region(terrain: Node) -> bool:
	for _frame in range(1800):
		_audit_frame(terrain)
		var metrics: Dictionary = terrain.call("get_runtime_metrics")
		if (
			int(metrics.get("pending_chunk_retirements", 0)) > 0 and
			int(metrics.get("pending_chunk_replacements", 0)) > 0
		):
			return true
		await create_timer(0.001).timeout
	return false


func _wait_for_settled(terrain: Node) -> bool:
	for _frame in range(1800):
		_audit_frame(terrain)
		var metrics: Dictionary = terrain.call("get_runtime_metrics")
		if (
			int(metrics.get("active_chunk_records", 0)) > 0 and
			int(metrics.get("queued_render", 0)) == 0 and
			int(metrics.get("queued_collision", 0)) == 0 and
			int(metrics.get("pending_chunk_retirements", 0)) == 0 and
			int(metrics.get("pending_chunk_replacements", 0)) == 0 and
			int(metrics.get("pending_render_retirements", 0)) == 0 and
			int(metrics.get("staged_render_resources", 0)) == 0 and
			int(metrics.get("staged_collision_resources", 0)) == 0 and
			int(metrics.get("fully_ready_chunk_records", -1)) ==
				int(metrics.get("active_chunk_records", 0))
		):
			await process_frame
			_audit_frame(terrain)
			return true
		await create_timer(0.001).timeout
	return false


func _audit_frame(terrain: Node) -> void:
	var metrics: Dictionary = terrain.call("get_runtime_metrics")
	maximum_staged_collision_resources = maxi(
		maximum_staged_collision_resources,
		int(metrics.get("staged_collision_resources", 0))
	)
	var renders: Array[Dictionary] = []
	var collisions: Array[Dictionary] = []
	for child in terrain.get_children():
		var child_name := str(child.name)
		if (
			child is MeshInstance3D and
			child.visible and
			child_name.begins_with("WT_Render_")
		):
			var render_key := _parse_key(child_name, "WT_Render_")
			if not render_key.is_empty():
				renders.append(render_key)
		elif (
			child is StaticBody3D and
			child_name.begins_with("WT_Collision_")
		):
			var collision_key := _parse_key(child_name, "WT_Collision_")
			if not collision_key.is_empty():
				collisions.append(collision_key)

	for collision in collisions:
		var matching_render := false
		for render in renders:
			if collision.id == render.id:
				matching_render = true
				break
		if not matching_render:
			visual_collision_mismatch_frames += 1
			if first_mismatch.is_empty():
				first_mismatch = (
					"collision=%s renders=%s metrics=%s" %
					[
						collision.id,
						_ids(renders),
						JSON.stringify(metrics),
					]
				)
			break

	for first_index in range(renders.size()):
		for second_index in range(first_index + 1, renders.size()):
			if renders[first_index].lod == renders[second_index].lod:
				continue
			if _positive_overlap(
				renders[first_index].bounds,
				renders[second_index].bounds
			):
				visible_cross_lod_overlap_frames += 1
				if first_mismatch.is_empty():
					first_mismatch = "render_overlap=%s/%s" % [
						renders[first_index].id,
						renders[second_index].id,
					]
				return

	for first_index in range(collisions.size()):
		for second_index in range(first_index + 1, collisions.size()):
			if _positive_overlap(
				collisions[first_index].bounds,
				collisions[second_index].bounds
			):
				collision_overlap_frames += 1
				if first_mismatch.is_empty():
					first_mismatch = "collision_overlap=%s/%s" % [
						collisions[first_index].id,
						collisions[second_index].id,
					]
				return


func _ids(records: Array[Dictionary]) -> String:
	var values: Array[String] = []
	for record in records:
		values.append(str(record.id))
	return ",".join(values)


func _parse_key(node_name: String, prefix: String) -> Dictionary:
	var parts := node_name.trim_prefix(prefix).split("_")
	if parts.size() != 4 or not parts[3].begins_with("L"):
		return {}
	var coordinate := Vector3i(
		int(parts[0]),
		int(parts[1]),
		int(parts[2])
	)
	var lod := int(parts[3].trim_prefix("L"))
	var extent := 16.0 * float(1 << lod)
	return {
		"id": "%d:%d:%d:%d" % [coordinate.x, coordinate.y, coordinate.z, lod],
		"lod": lod,
		"bounds": AABB(Vector3(coordinate) * extent, Vector3.ONE * extent),
	}


func _positive_overlap(first: AABB, second: AABB) -> bool:
	var overlap := first.intersection(second)
	return (
		overlap.size.x > 0.0001 and
		overlap.size.y > 0.0001 and
		overlap.size.z > 0.0001
	)


func _fail(message: String) -> void:
	push_error("PRODUCTION_GODOT_LOD_EDIT_ATOMICITY_FAIL: " + message)
	quit(1)
