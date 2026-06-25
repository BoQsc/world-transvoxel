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
	var geometry_error := _validate_godot_geometry(terrain)
	if not geometry_error.is_empty():
		return _fail(geometry_error)
	await physics_frame
	await physics_frame
	var ray := PhysicsRayQueryParameters3D.create(
		Vector3(8, 8, 24),
		Vector3(8, 8, 8)
	)
	if terrain.get_world_3d().direct_space_state.intersect_ray(ray).is_empty():
		return _fail("outside ray did not hit the front of the sphere collision")
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
	if terrain.get_render_resource_count() != 2 or \
			terrain.get_collision_resource_count() != 1:
		return _fail("resource replacement did not create bounded crossfade nodes")
	for frame in range(24):
		await process_frame
	if terrain.get_render_resource_count() != 1 or \
			terrain.get_collision_resource_count() != 1:
		return _fail("resource replacement crossfade did not settle")

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


func _validate_godot_geometry(terrain: Node) -> String:
	var render := terrain.get_node_or_null("WT_Render_0_0_0_L0") as MeshInstance3D
	if render == null or render.mesh == null or render.mesh.get_surface_count() != 1:
		return "Godot render mesh is missing"
	var arrays := render.mesh.surface_get_arrays(0)
	var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
	var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
	if positions.is_empty() or positions.size() != normals.size() or \
			indices.is_empty() or indices.size() % 3 != 0:
		return "Godot render arrays are invalid"
	for triangle in range(0, indices.size(), 3):
		var a_index := indices[triangle]
		var b_index := indices[triangle + 1]
		var c_index := indices[triangle + 2]
		var geometric := (
			positions[b_index] - positions[a_index]
		).cross(positions[c_index] - positions[a_index])
		var outward := (
			normals[a_index] + normals[b_index] + normals[c_index]
		)
		if geometric.length_squared() <= 0.0 or outward.length_squared() <= 0.0:
			return "Godot render mesh contains a degenerate orientation anchor"
		if geometric.normalized().dot(outward.normalized()) >= -0.5:
			return "Godot ArrayMesh front-face winding is not clockwise/outward"

	var body := terrain.get_node_or_null("WT_Collision_0_0_0_L0") as StaticBody3D
	if body == null:
		return "Godot collision body is missing"
	var collision := body.get_node_or_null("Shape") as CollisionShape3D
	if collision == null or not collision.shape is ConcavePolygonShape3D:
		return "Godot concave collision shape is missing"
	var shape := collision.shape as ConcavePolygonShape3D
	if shape.backface_collision:
		return "Godot collision unexpectedly accepts back faces"
	if shape.get_faces().is_empty():
		return "Godot collision shape has no faces"
	return ""


func _fail(message: String) -> void:
	push_error("M3_GODOT_INTEGRATION_FAIL: " + message)
	quit(1)
