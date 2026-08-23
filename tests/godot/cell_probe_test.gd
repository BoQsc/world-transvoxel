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
	var tables: Dictionary = probe.call("get_gpu_meshing_tables")
	if tables.get("schema", "") != "world_transvoxel.cell_probe.gpu_meshing_tables.v1":
		_fail("unexpected GPU meshing table schema")
		return
	if tables.get("authority", "") != "NATIVE_TRANSVOXEL_BACKEND_TABLE_EXPORT":
		_fail("GPU meshing tables were not marked as a native export")
		return
	var table_sizes := {
		"regular_cell_class": 256,
		"regular_cell_data": 16 * 16,
		"regular_vertex_data": 256 * 12,
		"transition_cell_class": 512,
		"transition_cell_data": 56 * 37,
		"transition_vertex_data": 512 * 12,
	}
	for table_name in table_sizes:
		var values := PackedInt32Array(tables.get(table_name, PackedInt32Array()))
		if values.size() != int(table_sizes[table_name]):
			_fail("unexpected %s size" % table_name)
			return
	if PackedInt32Array(tables["regular_cell_class"])[1] != 1 or \
			PackedInt32Array(tables["regular_vertex_data"])[12] != 0x6201 or \
			PackedInt32Array(tables["transition_cell_class"])[3] != 0x84 or \
			PackedInt32Array(tables["transition_vertex_data"])[12] != 0x2301:
		_fail("GPU meshing table sentinels differ from the native backend")
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

	var chunk_mesh: Dictionary = probe.call(
		"mesh_chunk_with_callable",
		Callable(self, "_chunk_sample"),
		Vector3i.ZERO,
		0,
		0,
		0,
		0.0,
		0.25
	)
	if chunk_mesh.get("schema", "") != "world_transvoxel.cell_probe.chunk_mesh.v1":
		_fail("unexpected chunk mesh schema")
		return
	if not bool(chunk_mesh.get("ok", false)):
		_fail(
			"chunk probe did not mesh ok: %s %s" %
			[str(chunk_mesh.get("status", "")), str(chunk_mesh.get("sample_error", ""))]
		)
		return
	var regular_chunk: Dictionary = chunk_mesh.get("regular", {})
	if int(regular_chunk.get("vertex_count", 0)) <= 0 or \
			int(regular_chunk.get("triangle_count", 0)) <= 0:
		_fail("chunk probe produced no regular geometry")
		return
	if int(chunk_mesh.get("transition_triangle_count", -1)) != 0:
		_fail("LOD 0 chunk probe unexpectedly produced transition geometry")
		return
	if PackedInt32Array(regular_chunk.get("indices", PackedInt32Array())).size() != \
			PackedInt32Array(regular_chunk.get("backend_indices", PackedInt32Array())).size():
		_fail("chunk render/backend index arrays differ in length")
		return

	print(
		"CELL_PROBE_TEST_PASS regular_vertices=%d regular_triangles=%d transition_vertices=%d transition_triangles=%d chunk_vertices=%d chunk_triangles=%d backend=%s" %
		[
			int(mesh.get("vertex_count", 0)),
			int(mesh.get("triangle_count", 0)),
			int(transition_mesh.get("vertex_count", 0)),
			int(transition_mesh.get("triangle_count", 0)),
			int(regular_chunk.get("vertex_count", 0)),
			int(regular_chunk.get("triangle_count", 0)),
			str(identity.get("backend_id", "")),
		]
	)
	quit(0)


func _chunk_sample(point: Vector3i) -> Dictionary:
	var p := Vector3(float(point.x), float(point.y), float(point.z)) - Vector3(8.0, 8.0, 8.0)
	var density := p.length() - 5.0
	return {
		"density": density,
		"material": 1 if density < 0.0 else 0,
		"material_authored": true,
	}


func _fail(message: String) -> void:
	push_error("CELL_PROBE_TEST_FAIL: " + message)
	quit(1)
