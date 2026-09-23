extends SceneTree


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var terrain: Node = ClassDB.instantiate("WorldTransvoxelTerrain")
	var config: Resource = ClassDB.instantiate("WorldTransvoxelConfig")
	if terrain == null or config == null:
		_fail("native terrain classes are unavailable")
		return
	root.add_child(terrain)
	terrain.set("configuration", config)
	if not terrain.call(
		"start_procedural_world_preset_with_vertical_origin_and_bottom_boundary",
		128, 16, -8, 128, 19023, 190327,
		"four_biomes_lakes_caves_roads", 2, 16,
		"res://build/base-atlas-upload-test-objects"
	):
		_fail("g23 source startup rejected: %s" % terrain.call("get_world_error"))
		return
	for frame in range(120):
		if str(terrain.call("get_world_state_name")) == "running":
			break
		await process_frame
	if str(terrain.call("get_world_state_name")) != "running":
		_fail("g23 source did not start")
		return
	var started_us := Time.get_ticks_usec()
	var upload: Dictionary = terrain.call(
		"_load_gpu_base_coverage_atlas",
		"res://addons/world_transvoxel/data/g23_base_lod3.wtba"
	)
	var prepare_ms := float(Time.get_ticks_usec() - started_us) / 1000.0
	if not bool(upload.get("ok", false)):
		_fail("atlas upload preparation rejected: %s" % upload.get("error", ""))
		return
	if int(upload.get("vertex_count", 0)) != 86049 \
			or int(upload.get("index_count", 0)) != 460344 \
			or int(upload.get("draw_count", 0)) != 266 \
			or Array(upload.get("roots", [])).size() != 512 \
			or PackedByteArray(upload.get("positions", PackedByteArray())).size() != 86049 * 12:
		_fail("atlas upload inventory disagrees with native bake")
		return
	if not terrain.call("stop_world"):
		_fail("g23 source did not stop")
		return
	print("WT_GODOT_BASE_ATLAS_UPLOAD_PASS roots=512 draws=266 prepare_ms=%.3f" % [
		prepare_ms
	])
	terrain.queue_free()
	await process_frame
	quit(0)


func _fail(message: String) -> void:
	push_error("WT_GODOT_BASE_ATLAS_UPLOAD_FAIL: " + message)
	quit(1)
