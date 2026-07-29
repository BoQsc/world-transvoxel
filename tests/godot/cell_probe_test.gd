extends SceneTree


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	if not ClassDB.class_exists("WorldTransvoxelCellProbe"):
		_fail("WorldTransvoxelCellProbe was not registered")
		return

	var probe: RefCounted = ClassDB.instantiate("WorldTransvoxelCellProbe")
	if probe == null:
		_fail("WorldTransvoxelCellProbe could not be instantiated")
		return

	var identity: Dictionary = probe.call("get_backend_identity")
	if identity.get("schema", "") != "world_transvoxel.cell_probe.identity.v1":
		_fail("unexpected identity schema")
		return
	if identity.get("backend_id", "") != "transvoxel_mit_official":
		_fail("unexpected backend id")
		return
	if not bool(identity.get("available", false)):
		_fail("backend was not available")
		return

	var densities := PackedFloat32Array([-1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0])
	var gradients := PackedVector3Array()
	var materials := PackedInt32Array([1, 2, 3, 4, 5, 6, 7, 8])
	for _index in range(8):
		gradients.append(Vector3(1.0, 0.0, 0.0))

	var mesh: Dictionary = probe.call(
		"mesh_regular_cell",
		densities,
		gradients,
		materials,
		Vector3.ZERO,
		1.0,
		0.0
	)
	if mesh.get("schema", "") != "world_transvoxel.cell_probe.mesh.v1":
		_fail("unexpected mesh schema")
		return
	if mesh.get("render_authority", "") != "NATIVE_TRANSVOXEL_BACKEND_AUTHORITATIVE":
		_fail("mesh was not marked authoritative")
		return
	if int(mesh.get("case_code", -1)) != 1:
		_fail("unexpected regular case code")
		return
	if not bool(mesh.get("ok", false)):
		_fail("regular cell did not mesh ok: %s" % str(mesh.get("status", "")))
		return
	if int(mesh.get("vertex_count", 0)) <= 0 or int(mesh.get("triangle_count", 0)) <= 0:
		_fail("regular cell produced no geometry")
		return
	if PackedInt32Array(mesh.get("indices", PackedInt32Array())).size() != \
			PackedInt32Array(mesh.get("backend_indices", PackedInt32Array())).size():
		_fail("render/backend index arrays differ in length")
		return

	var transition_densities := PackedFloat32Array([
		-0.9, -0.9, -0.9,
		0.1, 0.1, 0.1,
		1.1, 1.1, 1.1,
	])
	var transition_gradients := PackedVector3Array()
	var transition_materials := PackedInt32Array([1, 1, 1, 0, 0, 0, 0, 0, 0])
	for _index in range(9):
		transition_gradients.append(Vector3(0.0, 1.0, 0.0))
	var transition_mesh: Dictionary = probe.call(
		"mesh_transition_cell",
		transition_densities,
		transition_gradients,
		transition_materials,
		4,
		Vector3.ZERO,
		1.0,
		0.75,
		0.0
	)
	if not bool(transition_mesh.get("ok", false)):
		_fail("transition cell did not mesh ok: %s" % str(transition_mesh.get("status", "")))
		return
	if int(transition_mesh.get("vertex_count", 0)) <= 0 or \
			int(transition_mesh.get("triangle_count", 0)) <= 0:
		_fail("transition cell produced no geometry")
		return
	if int(transition_mesh.get("orientation", -1)) != 4:
		_fail("transition orientation was not preserved")
		return

	print(
		"CELL_PROBE_TEST_PASS regular_vertices=%d regular_triangles=%d transition_vertices=%d transition_triangles=%d backend=%s" %
		[
			int(mesh.get("vertex_count", 0)),
			int(mesh.get("triangle_count", 0)),
			int(transition_mesh.get("vertex_count", 0)),
			int(transition_mesh.get("triangle_count", 0)),
			str(identity.get("backend_id", "")),
		]
	)
	quit(0)


func _fail(message: String) -> void:
	push_error("CELL_PROBE_TEST_FAIL: " + message)
	quit(1)
