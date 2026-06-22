extends SceneTree


const SoakSupport := preload(
	"res://tests/godot/production_full_world_soak_support.gd"
)
const SOURCE_REVISION := 8001
const COMPACTED_SOURCE_REVISION := 8002
const WORLD_PAGE_COUNT := 28
const DEFAULT_DURATION_MS := 15000
const MAX_MEMORY_BYTES := 805306368
const MAX_FRAME_US := 2000000
const MAX_P99_FRAME_US := 250000

var terrain: Node
var sample_results: Dictionary = {}
var sample_failures: Dictionary = {}
var snapshot_results: Dictionary = {}
var snapshot_failures: Dictionary = {}
var committed_revisions: Dictionary = {}
var edit_failures: Array[String] = []
var observed_render_chunks: Dictionary = {}
var observed_collision_chunks: Dictionary = {}
var frame_samples_us: Array[int] = []
var frame_count := 0
var last_frame_us := 0
var maximum_frame_us := 0
var maximum_memory_bytes := 0
var maximum_render_resources := 0
var maximum_collision_resources := 0
var maximum_render_queue := 0
var maximum_collision_queue := 0
var viewer_revisions := {1: 0, 2: 0}
var viewer_active := {1: false, 2: false}
var query_count := 0
var movement_steps := 0
var edit_count := 0
var transition_checks := 0


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	var duration_ms := SoakSupport.duration_argument(DEFAULT_DURATION_MS)
	var config: Resource = ClassDB.instantiate("WorldTransvoxelConfig")
	terrain = ClassDB.instantiate("WorldTransvoxelTerrain")
	if config == null or terrain == null or \
			not ClassDB.class_exists("WorldTransvoxelSample"):
		_fail("clean-installed production classes are unavailable")
		return
	if terrain.call("get_backend_id") != "transvoxel_mit_official" or \
			terrain.call("get_backend_license") != "MIT":
		_fail("official MIT backend identity is unavailable")
		return
	SoakSupport.configure(config)
	root.add_child(terrain)
	terrain.set("configuration", config)
	terrain.connect("authoritative_sample_ready", _on_sample_ready)
	terrain.connect("authoritative_sample_failed", _on_sample_failed)
	terrain.connect("world_snapshot_ready", _on_snapshot_ready)
	terrain.connect("world_snapshot_failed", _on_snapshot_failed)
	terrain.connect("edit_committed", _on_edit_committed)
	terrain.connect("edit_failed", _on_edit_failed)

	const world_root := "res://world"
	const world_path := world_root + "/world.wtworld"
	const compacted_root := "res://outputs/compacted"
	const migrated_root := "res://outputs/migrated"
	var outputs_absolute := ProjectSettings.globalize_path("res://outputs")
	SoakSupport.remove_tree(outputs_absolute)
	if DirAccess.make_dir_absolute(outputs_absolute) != OK:
		_fail("full-world snapshot parent could not be created")
		return
	if not terrain.call("start_world", world_path, world_root) or \
			not await _wait_for_state("running"):
		_fail("clean-installed baked world did not start")
		return
	if terrain.call("get_world_page_count") != WORLD_PAGE_COUNT or \
			terrain.call("get_world_source_revision") != SOURCE_REVISION or \
			terrain.call("get_world_revision") != 0:
		_fail("clean-installed baked world metadata mismatch")
		return

	if not _update_viewer(1, Vector3(8, 8, 8)) or \
			not await _wait_for_counts(5, 5):
		_fail("initial full-world streaming state did not settle")
		return
	if not await _query_every_baked_page():
		return
	if not await _commit_and_query_material(Vector3i(8, 8, 8), 20):
		return

	last_frame_us = Time.get_ticks_usec()
	var soak_started_ms := Time.get_ticks_msec()
	var states := [
		{"action": "update", "viewer": 2, "position": Vector3(80, 8, 8), "count": 10},
		{"action": "update", "viewer": 1, "position": Vector3(40, 8, 8), "count": 13},
		{"action": "remove", "viewer": 2, "count": 9},
		{"action": "update", "viewer": 1, "position": Vector3(80, 8, 8), "count": 6},
		{"action": "update", "viewer": 1, "position": Vector3(8, 8, 8), "count": 5},
	]
	while Time.get_ticks_msec() - soak_started_ms < duration_ms:
		var state: Dictionary = states[movement_steps % states.size()]
		var accepted := false
		if state.action == "update":
			accepted = _update_viewer(state.viewer, state.position)
		else:
			accepted = _remove_viewer(state.viewer)
		if not accepted or not await _wait_for_counts(state.count, state.count):
			_fail("full-world movement state did not settle")
			return
		movement_steps += 1
		if not _verify_transition_state(state.count):
			_fail("balanced LOD transition identity or readiness mismatch")
			return
		if edit_count < 12 and movement_steps % 2 == 0:
			var point: Vector3i = [
				Vector3i(8, 8, 8),
				Vector3i(40, 8, 8),
				Vector3i(80, 8, 8),
			][edit_count % 3]
			if not await _commit_and_query_material(point, 20 + edit_count):
				return
	var actual_duration_ms := Time.get_ticks_msec() - soak_started_ms
	if actual_duration_ms < duration_ms:
		_fail("full-world soak ended before the requested duration")
		return

	var compact_id: int = terrain.call(
		"request_world_compaction", compacted_root, COMPACTED_SOURCE_REVISION
	)
	if compact_id <= 0 or not await _wait_for_snapshot(compact_id):
		_fail("full-world compaction did not complete")
		return
	var compacted: Dictionary = snapshot_results[compact_id]
	if compacted.source_revision != COMPACTED_SOURCE_REVISION or \
			compacted.world_revision != edit_count or \
			compacted.page_count != WORLD_PAGE_COUNT:
		_fail("full-world compaction metadata mismatch")
		return
	var source_metrics: Dictionary = terrain.call("get_runtime_metrics")
	if not _validate_source_metrics(source_metrics):
		return

	if not _remove_viewer(1) or not _remove_viewer(2) or \
			not await _wait_for_counts(0, 0) or \
			not terrain.call("stop_world") or \
			not await _wait_for_state("stopped"):
		_fail("source full-world lifecycle did not stop cleanly")
		return
	if not _resources_are_empty():
		_fail("source shutdown retained resources or application queues")
		return

	_reset_async_results()
	if not terrain.call(
			"start_world",
			compacted.manifest_path,
			ProjectSettings.globalize_path(compacted_root)
		) or not await _wait_for_state("running"):
		_fail("compacted full world did not reopen")
		return
	if terrain.call("get_world_source_revision") != COMPACTED_SOURCE_REVISION or \
			terrain.call("get_world_revision") != edit_count or \
			not _update_viewer(1, Vector3(8, 8, 8)) or \
			not await _wait_for_counts(5, 5):
		_fail("compacted full-world state mismatch")
		return
	var final_material := 20 + _last_edit_index_for_point(Vector3i(8, 8, 8))
	if not await _query_material(Vector3i(8, 8, 8), 0, final_material):
		return

	var migrate_id: int = terrain.call(
		"request_world_migration", migrated_root
	)
	if migrate_id <= 0 or not await _wait_for_snapshot(migrate_id):
		_fail("compacted full-world migration did not complete")
		return
	var migrated: Dictionary = snapshot_results[migrate_id]
	if migrated.source_revision != COMPACTED_SOURCE_REVISION or \
			migrated.world_revision != edit_count or \
			migrated.page_count != WORLD_PAGE_COUNT:
		_fail("full-world migration metadata mismatch")
		return
	var compacted_metrics: Dictionary = terrain.call("get_runtime_metrics")
	if not _validate_snapshot_runtime(compacted_metrics, "compacted"):
		return
	if not _remove_viewer(1) or not _remove_viewer(2) or \
			not await _wait_for_counts(0, 0) or \
			not terrain.call("stop_world") or \
			not await _wait_for_state("stopped"):
		_fail("compacted full world did not stop cleanly")
		return

	_reset_async_results()
	if not terrain.call(
			"start_world",
			migrated.manifest_path,
			ProjectSettings.globalize_path(migrated_root)
		) or not await _wait_for_state("running"):
		_fail("migrated full world did not reopen")
		return
	if not await _query_material(Vector3i(8, 8, 8), 0, final_material):
		return
	var migrated_metrics: Dictionary = terrain.call("get_runtime_metrics")
	if migrated_metrics.sample_queries != 1 or \
			migrated_metrics.sample_query_rejections != 0 or \
			migrated_metrics.rejected_events != 0:
		_fail("migrated full-world runtime metrics mismatch")
		return
	if not terrain.call("stop_world") or \
			not await _wait_for_state("stopped") or \
			not _resources_are_empty():
		_fail("migrated full-world shutdown retained resources")
		return

	var p99_frame_us := SoakSupport.percentile_frame_us(
		frame_samples_us, 0.99
	)
	if maximum_memory_bytes > MAX_MEMORY_BYTES or \
			maximum_frame_us > MAX_FRAME_US or \
			p99_frame_us > MAX_P99_FRAME_US:
		_fail("full-world memory or frame budget exceeded")
		return
	if maximum_render_resources > 40 or maximum_collision_resources > 40 or \
			maximum_render_queue > 40 or maximum_collision_queue > 40:
		_fail("full-world resource or queue bound exceeded")
		return
	if observed_render_chunks.size() < 10 or \
			observed_collision_chunks.size() < 10 or transition_checks < 4:
		_fail("full-world render/collision/transition coverage is incomplete")
		return

	var evidence := {
		"schema": 1,
		"scope": "clean_install_full_world_godot_soak",
		"requested_duration_ms": duration_ms,
		"actual_duration_ms": actual_duration_ms,
		"world_pages": WORLD_PAGE_COUNT,
		"queried_pages": WORLD_PAGE_COUNT,
		"movement_steps": movement_steps,
		"edits": edit_count,
		"queries": query_count,
		"transition_checks": transition_checks,
		"observed_render_chunks": observed_render_chunks.size(),
		"observed_collision_chunks": observed_collision_chunks.size(),
		"frame_count": frame_count,
		"frame_sample_count": frame_samples_us.size(),
		"maximum_frame_us": maximum_frame_us,
		"p99_frame_us": p99_frame_us,
		"maximum_memory_bytes": maximum_memory_bytes,
		"maximum_render_resources": maximum_render_resources,
		"maximum_collision_resources": maximum_collision_resources,
		"maximum_render_queue": maximum_render_queue,
		"maximum_collision_queue": maximum_collision_queue,
		"source_runtime": source_metrics,
		"compacted_runtime": compacted_metrics,
		"migrated_runtime": migrated_metrics,
		"shutdown": "clean",
	}
	print("PRODUCTION_GODOT_FULL_WORLD_SOAK_PASS " + JSON.stringify(evidence))
	terrain.queue_free()
	await process_frame
	quit(0)


func _query_every_baked_page() -> bool:
	for z in range(2):
		for y in range(2):
			for x in range(6):
				if not await _query_material(
					Vector3i(x * 16 + 8, y * 16 + 8, z * 16 + 8),
					0,
					7
				):
					return false
	for x in range(4):
		if not await _query_material(Vector3i(x * 32 + 16, 16, 16), 1, 7):
			return false
	return true


func _commit_and_query_material(point: Vector3i, material: int) -> bool:
	var transaction: RefCounted = terrain.call(
		"begin_edit_transaction", 700 + edit_count
	)
	if transaction == null:
		_fail("full-world material edit transaction could not begin")
		return false
	var constructed: bool = transaction.call(
		"paint_material_box",
		Vector3(point) - Vector3(2, 2, 2),
		Vector3(point) + Vector3(2, 2, 2),
		material
	)
	var committed: bool = constructed and terrain.call(
		"commit_edit_transaction", transaction
	)
	if not constructed or not committed:
		_fail(
			"full-world material edit submission failed transaction=%s world=%s" % [
				transaction.call("get_error"),
				terrain.call("get_world_error"),
			]
		)
		return false
	edit_count += 1
	var request_id: int = terrain.call(
		"request_authoritative_sample", point, 0
	)
	query_count += 1
	if request_id <= 0 or \
			not await _wait_for_commit_and_sample(edit_count, request_id):
		_fail("full-world edit/query ordering did not complete")
		return false
	var sample: RefCounted = sample_results[request_id]
	if sample.call("get_material") != material or \
			sample.call("get_world_revision") != edit_count:
		_fail("full-world edited authoritative sample mismatch")
		return false
	return true


func _query_material(point: Vector3i, lod: int, material: int) -> bool:
	var request_id: int = terrain.call(
		"request_authoritative_sample", point, lod
	)
	query_count += 1
	if request_id <= 0 or not await _wait_for_sample(request_id):
		_fail("full-world authoritative page query failed")
		return false
	var sample: RefCounted = sample_results[request_id]
	if sample.call("get_grid_point") != point or \
			sample.call("get_lod") != lod or \
			sample.call("get_material") != material or \
			sample.call("get_agreeing_page_count") <= 0:
		_fail("full-world authoritative page sample mismatch")
		return false
	return true


func _last_edit_index_for_point(point: Vector3i) -> int:
	for index in range(edit_count - 1, -1, -1):
		var edited_point: Vector3i = [
			Vector3i(8, 8, 8),
			Vector3i(40, 8, 8),
			Vector3i(80, 8, 8),
		][index % 3]
		if edited_point == point:
			return index
	return 0


func _update_viewer(viewer_id: int, position: Vector3) -> bool:
	viewer_revisions[viewer_id] += 1
	var accepted: bool = terrain.call(
		"update_viewer",
		viewer_id,
		viewer_revisions[viewer_id],
		position,
		1,
		1
	)
	if accepted:
		viewer_active[viewer_id] = true
	return accepted


func _remove_viewer(viewer_id: int) -> bool:
	if not viewer_active[viewer_id]:
		return true
	viewer_revisions[viewer_id] += 1
	var accepted: bool = terrain.call(
		"remove_viewer", viewer_id, viewer_revisions[viewer_id]
	)
	if accepted:
		viewer_active[viewer_id] = false
	return accepted


func _verify_transition_state(expected_count: int) -> bool:
	var expected_names: Array[String] = []
	match expected_count:
		5:
			expected_names = ["WT_Render_0_0_0_L0", "WT_Render_1_0_0_L1"]
		6:
			expected_names = ["WT_Render_4_0_0_L0", "WT_Render_3_0_0_L1"]
		9:
			expected_names = ["WT_Render_2_0_0_L0", "WT_Render_2_0_0_L1"]
		10:
			expected_names = ["WT_Render_1_0_0_L1", "WT_Render_3_0_0_L1"]
		13:
			expected_names = ["WT_Render_2_0_0_L0", "WT_Render_3_0_0_L1"]
	for name in expected_names:
		if terrain.get_node_or_null(name) == null:
			return false
		transition_checks += 1
	return true


func _validate_source_metrics(metrics: Dictionary) -> bool:
	if metrics.viewer_updates + metrics.viewer_removals < movement_steps + 1 or \
			metrics.viewer_removals < 1 or \
			metrics.storage_completions <= 0 or metrics.mesh_completions <= 0 or \
			metrics.transition_mesh_completions <= 0 or \
			metrics.edit_commits != edit_count or metrics.edit_replacements <= 0 or \
			metrics.sample_queries < WORLD_PAGE_COUNT + edit_count or \
			metrics.world_snapshots != 1:
		_fail(
			"source full-world runtime coverage metrics are incomplete: " +
			JSON.stringify(metrics)
		)
		return false
	if metrics.edit_rejections != 0 or \
			metrics.sample_query_rejections != 0 or \
			metrics.world_snapshot_rejections != 0 or \
			metrics.rejected_events != 0 or \
			metrics.application_sink_failures != 0 or \
			metrics.application_queue_rejections != 0:
		_fail("source full-world runtime reported a rejection or failure")
		return false
	if metrics.render_latency_frames_maximum > 32 or \
			metrics.collision_latency_frames_maximum > 32:
		_fail("source full-world application latency bound exceeded")
		return false
	return true


func _validate_snapshot_runtime(metrics: Dictionary, label: String) -> bool:
	if metrics.sample_queries != 1 or metrics.world_snapshots != 1 or \
			metrics.sample_query_rejections != 0 or \
			metrics.world_snapshot_rejections != 0 or \
			metrics.rejected_events != 0:
		_fail(label + " full-world runtime metrics mismatch")
		return false
	return true


func _wait_for_state(expected: String) -> bool:
	for _frame in range(1800):
		if terrain.call("get_world_state_name") == expected:
			await _advance_frame()
			return true
		if terrain.call("get_world_state_name") == "failed":
			return false
		await _advance_frame()
	return false


func _wait_for_counts(render_count: int, collision_count: int) -> bool:
	for _frame in range(1800):
		var metrics: Dictionary = terrain.call("get_runtime_metrics")
		if terrain.call("get_rendered_chunk_count") == render_count and \
				terrain.call("get_collision_chunk_count") == collision_count and \
				terrain.call("get_queued_render_count") == 0 and \
				terrain.call("get_queued_collision_count") == 0 and \
				int(metrics.get("fully_ready_chunk_records", -1)) == \
				int(metrics.get("active_chunk_records", 0)) and \
				int(metrics.get("pending_chunk_retirements", 0)) == 0:
			await _advance_frame()
			return true
		if terrain.call("get_world_state_name") == "failed":
			return false
		await _advance_frame()
	return false


func _wait_for_sample(request_id: int) -> bool:
	for _frame in range(1800):
		if sample_results.has(request_id):
			return true
		if sample_failures.has(request_id):
			return false
		await _advance_frame()
	return false


func _wait_for_commit_and_sample(revision: int, request_id: int) -> bool:
	for _frame in range(1800):
		if committed_revisions.has(revision) and sample_results.has(request_id):
			return true
		if sample_failures.has(request_id) or not edit_failures.is_empty():
			return false
		await _advance_frame()
	return false


func _wait_for_snapshot(request_id: int) -> bool:
	for _frame in range(3600):
		if snapshot_results.has(request_id):
			return true
		if snapshot_failures.has(request_id):
			return false
		await _advance_frame()
	return false


func _advance_frame() -> void:
	await process_frame
	var now := Time.get_ticks_usec()
	if last_frame_us > 0:
		var elapsed := now - last_frame_us
		maximum_frame_us = maxi(maximum_frame_us, elapsed)
		frame_count += 1
		if frame_count % 8 == 0 and frame_samples_us.size() < 65536:
			frame_samples_us.push_back(elapsed)
	last_frame_us = now
	maximum_memory_bytes = maxi(
		maximum_memory_bytes, int(OS.get_static_memory_peak_usage())
	)
	if terrain != null:
		maximum_render_resources = maxi(
			maximum_render_resources,
			terrain.call("get_render_resource_count")
		)
		maximum_collision_resources = maxi(
			maximum_collision_resources,
			terrain.call("get_collision_resource_count")
		)
		maximum_render_queue = maxi(
			maximum_render_queue,
			terrain.call("get_queued_render_count")
		)
		maximum_collision_queue = maxi(
			maximum_collision_queue,
			terrain.call("get_queued_collision_count")
		)
		for child in terrain.get_children():
			var child_name := String(child.name)
			if child_name.begins_with("WT_Render_"):
				observed_render_chunks[child_name] = true
			elif child_name.begins_with("WT_Collision_"):
				observed_collision_chunks[child_name] = true


func _resources_are_empty() -> bool:
	return terrain.call("get_render_resource_count") == 0 and \
		terrain.call("get_collision_resource_count") == 0 and \
		terrain.call("get_queued_render_count") == 0 and \
		terrain.call("get_queued_collision_count") == 0


func _reset_async_results() -> void:
	sample_results.clear()
	sample_failures.clear()
	snapshot_results.clear()
	snapshot_failures.clear()
	committed_revisions.clear()
	edit_failures.clear()
	viewer_revisions = {1: 0, 2: 0}
	viewer_active = {1: false, 2: false}


func _on_sample_ready(request_id: int, sample: RefCounted) -> void:
	sample_results[request_id] = sample


func _on_sample_failed(request_id: int, error: String) -> void:
	sample_failures[request_id] = error


func _on_snapshot_ready(
	request_id: int,
	manifest_path: String,
	source_revision: int,
	world_revision: int,
	page_count: int
) -> void:
	snapshot_results[request_id] = {
		"manifest_path": manifest_path,
		"source_revision": source_revision,
		"world_revision": world_revision,
		"page_count": page_count,
	}


func _on_snapshot_failed(request_id: int, error: String) -> void:
	snapshot_failures[request_id] = error


func _on_edit_committed(world_revision: int) -> void:
	committed_revisions[world_revision] = true


func _on_edit_failed(error: String) -> void:
	edit_failures.push_back(error)


func _fail(message: String) -> void:
	push_error("PRODUCTION_GODOT_FULL_WORLD_SOAK_FAIL: " + message)
	quit(1)
