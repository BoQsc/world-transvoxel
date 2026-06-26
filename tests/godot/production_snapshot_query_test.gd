extends SceneTree


var sample_results: Dictionary = {}
var sample_failures: Dictionary = {}
var sample_batch_results: Dictionary = {}
var sample_batch_failures: Dictionary = {}
var snapshot_results: Dictionary = {}
var snapshot_failures: Dictionary = {}
var committed_revisions: Array[int] = []


func _initialize() -> void:
	call_deferred("_run_test")


func _run_test() -> void:
	var config: Resource = ClassDB.instantiate("WorldTransvoxelConfig")
	var terrain: Node = ClassDB.instantiate("WorldTransvoxelTerrain")
	if config == null or terrain == null or \
			not ClassDB.class_exists("WorldTransvoxelSample"):
		_fail("snapshot/query classes could not be instantiated")
		return
	root.add_child(terrain)
	terrain.set("configuration", config)
	terrain.connect("authoritative_sample_ready", _on_sample_ready)
	terrain.connect("authoritative_sample_failed", _on_sample_failed)
	terrain.connect("authoritative_samples_ready", _on_sample_batch_ready)
	terrain.connect("authoritative_samples_failed", _on_sample_batch_failed)
	terrain.connect("world_snapshot_ready", _on_snapshot_ready)
	terrain.connect("world_snapshot_failed", _on_snapshot_failed)
	terrain.connect("edit_committed", _on_edit_committed)

	const fixture_root := "res://build/production-lifecycle-fixture"
	const world_path := fixture_root + "/streaming.wtworld"
	const journal_path := fixture_root + "/world.wtedit"
	const compacted_root := "res://build/production-compacted-fixture"
	const migrated_root := "res://build/production-migrated-fixture"
	const legacy_migrated_root := \
		"res://build/production-legacy-migrated-fixture"
	_remove_tree(ProjectSettings.globalize_path(compacted_root))
	_remove_tree(ProjectSettings.globalize_path(migrated_root))
	_remove_tree(ProjectSettings.globalize_path(legacy_migrated_root))
	if FileAccess.file_exists(journal_path):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(journal_path))

	if not terrain.call("start_world", world_path, fixture_root) or \
			not await _wait_for_state(terrain, "running"):
		_fail("snapshot/query source world did not reach running")
		return

	var initial_id: int = terrain.call(
		"request_authoritative_sample", Vector3i(8, 8, 8), 0
	)
	if initial_id <= 0 or not await _wait_for_sample(initial_id):
		_fail("initial authoritative query did not complete")
		return
	var initial: RefCounted = sample_results[initial_id]
	if initial.call("get_grid_point") != Vector3i(8, 8, 8) or \
			initial.call("get_density") != -0.25 or \
			initial.call("get_material") != 7 or \
			initial.call("get_source_revision") != 7001 or \
			initial.call("get_world_revision") != 12:
		_fail("initial authoritative sample mismatch")
		return
	var batch_id: int = terrain.call(
		"request_authoritative_samples",
		[Vector3i(8, 8, 8), Vector3i(9, 8, 8), Vector3i(10, 8, 8)],
		0
	)
	if batch_id <= 0 or not await _wait_for_sample_batch(batch_id):
		_fail("batch authoritative query did not complete")
		return
	var batch: Array = sample_batch_results[batch_id]
	if batch.size() != 3:
		_fail("batch authoritative query returned the wrong count")
		return
	for index in range(batch.size()):
		var sample: RefCounted = batch[index]
		if sample.call("get_grid_point") != Vector3i(8 + index, 8, 8) or \
				sample.call("get_source_revision") != 7001 or \
				sample.call("get_world_revision") != 12:
			_fail("batch authoritative sample order or revision mismatch")
			return
	if batch[0].call("get_density") != initial.call("get_density") or \
			batch[0].call("get_material") != initial.call("get_material"):
		_fail("batch authoritative sample payload mismatch")
		return
	var invalid_id: int = terrain.call(
		"request_authoritative_sample", Vector3i(1, 8, 8), 1
	)
	if invalid_id <= 0 or not await _wait_for_sample_failure(invalid_id) or \
			sample_failures[invalid_id] != "sample point or LOD is invalid":
		_fail("misaligned authoritative query was not rejected")
		return

	var transaction: RefCounted = terrain.call("begin_edit_transaction", 91)
	if transaction == null or not transaction.call(
		"set_density_box",
		Vector3(-2, -2, -2),
		Vector3(18, 18, 18),
		10.0
	) or not terrain.call("commit_edit_transaction", transaction):
		_fail("snapshot/query edit submission failed")
		return
	var edited_id: int = terrain.call(
		"request_authoritative_sample", Vector3i(8, 8, 8), 0
	)
	if edited_id <= 0 or not await _wait_for_commit_and_sample(13, edited_id):
		_fail("edit/query operation ordering failed")
		return
	var edited: RefCounted = sample_results[edited_id]
	if edited.call("get_density") != 10.0 or \
			edited.call("get_world_revision") != 13:
		_fail("edited authoritative sample mismatch")
		return

	var rejected_migration: int = terrain.call(
		"request_world_migration", migrated_root
	)
	if rejected_migration <= 0 or \
			not await _wait_for_snapshot_failure(rejected_migration) or \
			snapshot_failures[rejected_migration] != \
			"world migration requires an empty edit journal":
		_fail("migration accepted a nonempty edit journal")
		return
	var compact_id: int = terrain.call(
		"request_world_compaction", compacted_root, 7002
	)
	if compact_id <= 0 or not await _wait_for_snapshot(compact_id):
		_fail("public world compaction did not complete")
		return
	var compacted: Dictionary = snapshot_results[compact_id]
	if compacted.source_revision != 7002 or \
			compacted.world_revision != 13 or compacted.page_count != 4 or \
			not FileAccess.file_exists(compacted.manifest_path):
		_fail("public compacted snapshot metadata mismatch")
		return

	if not terrain.call("stop_world") or \
			not await _wait_for_state(terrain, "stopped") or \
			not terrain.call(
				"start_world", compacted.manifest_path,
				ProjectSettings.globalize_path(compacted_root)
			) or not await _wait_for_state(terrain, "running"):
		_fail("compacted snapshot did not reopen")
		return
	sample_results.clear()
	sample_failures.clear()
	snapshot_results.clear()
	snapshot_failures.clear()
	var compacted_sample_id: int = terrain.call(
		"request_authoritative_sample", Vector3i(8, 8, 8), 0
	)
	if compacted_sample_id <= 0 or \
			not await _wait_for_sample(compacted_sample_id):
		_fail("compacted authoritative query did not complete")
		return
	var compacted_sample: RefCounted = sample_results[compacted_sample_id]
	if compacted_sample.call("get_density") != 10.0 or \
			compacted_sample.call("get_source_revision") != 7002 or \
			compacted_sample.call("get_world_revision") != 13:
		_fail("compacted authoritative sample mismatch")
		return

	var migrate_id: int = terrain.call(
		"request_world_migration", migrated_root
	)
	if migrate_id <= 0 or not await _wait_for_snapshot(migrate_id):
		_fail("public world migration did not complete")
		return
	var migrated: Dictionary = snapshot_results[migrate_id]
	if migrated.source_revision != 7002 or \
			migrated.world_revision != 13 or migrated.page_count != 4:
		_fail("public migrated snapshot metadata mismatch")
		return
	if not terrain.call("stop_world") or \
			not await _wait_for_state(terrain, "stopped") or \
			not terrain.call(
				"start_world", migrated.manifest_path,
				ProjectSettings.globalize_path(migrated_root)
			) or not await _wait_for_state(terrain, "running"):
		_fail("migrated snapshot did not reopen")
		return
	sample_results.clear()
	var migrated_sample_id: int = terrain.call(
		"request_authoritative_sample", Vector3i(8, 8, 8), 0
	)
	if migrated_sample_id <= 0 or \
			not await _wait_for_sample(migrated_sample_id) or \
			sample_results[migrated_sample_id].call("get_density") != 10.0:
		_fail("migrated authoritative sample mismatch")
		return

	if not terrain.call("stop_world") or \
			not await _wait_for_state(terrain, "stopped"):
		_fail("migrated snapshot did not stop cleanly")
		return
	if FileAccess.file_exists(journal_path):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(journal_path))
	sample_results.clear()
	sample_failures.clear()
	snapshot_results.clear()
	snapshot_failures.clear()
	if not terrain.call(
			"start_world", fixture_root + "/legacy.wtworld", fixture_root
		) or not await _wait_for_state(terrain, "running") or \
			terrain.call("get_world_source_revision") != 9001 or \
			terrain.call("get_world_revision") != 0:
		_fail("legacy schema-1.0 world did not open")
		return
	var legacy_migrate_id: int = terrain.call(
		"request_world_migration", legacy_migrated_root
	)
	if legacy_migrate_id <= 0 or \
			not await _wait_for_snapshot(legacy_migrate_id):
		_fail("legacy lifecycle migration did not complete")
		return
	var legacy_migrated: Dictionary = snapshot_results[legacy_migrate_id]
	if legacy_migrated.source_revision != 9001 or \
			legacy_migrated.world_revision != 0 or \
			legacy_migrated.page_count != 4:
		_fail("legacy lifecycle migration metadata mismatch")
		return
	if not terrain.call("stop_world") or \
			not await _wait_for_state(terrain, "stopped") or \
			not terrain.call(
				"start_world", legacy_migrated.manifest_path,
				ProjectSettings.globalize_path(legacy_migrated_root)
			) or not await _wait_for_state(terrain, "running"):
		_fail("migrated legacy snapshot did not reopen")
		return
	sample_results.clear()
	var legacy_sample_id: int = terrain.call(
		"request_authoritative_sample", Vector3i(8, 8, 8), 0
	)
	if legacy_sample_id <= 0 or \
			not await _wait_for_sample(legacy_sample_id):
		_fail("migrated legacy authoritative query did not complete")
		return
	var legacy_sample: RefCounted = sample_results[legacy_sample_id]
	if legacy_sample.call("get_density") != -0.25 or \
			legacy_sample.call("get_source_revision") != 9001 or \
			legacy_sample.call("get_world_revision") != 0:
		_fail("migrated legacy authoritative sample mismatch")
		return
	if not terrain.call("stop_world") or \
			not await _wait_for_state(terrain, "stopped"):
		_fail("snapshot/query world did not stop cleanly")
		return
	_remove_tree(ProjectSettings.globalize_path(compacted_root))
	_remove_tree(ProjectSettings.globalize_path(migrated_root))
	_remove_tree(ProjectSettings.globalize_path(legacy_migrated_root))
	if FileAccess.file_exists(journal_path):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(journal_path))
	print(
		"PRODUCTION_GODOT_SNAPSHOT_QUERY_PASS queries=8 compaction=1 " +
		"migrations=2 legacy=1 ordering=1"
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


func _wait_for_sample(request_id: int) -> bool:
	for _frame in range(900):
		if sample_results.has(request_id):
			return true
		if sample_failures.has(request_id):
			return false
		await process_frame
	return false


func _wait_for_sample_failure(request_id: int) -> bool:
	for _frame in range(900):
		if sample_failures.has(request_id):
			return true
		await process_frame
	return false


func _wait_for_sample_batch(request_id: int) -> bool:
	for _frame in range(900):
		if sample_batch_results.has(request_id):
			return true
		if sample_batch_failures.has(request_id):
			return false
		await process_frame
	return false


func _wait_for_commit_and_sample(revision: int, request_id: int) -> bool:
	for _frame in range(900):
		if committed_revisions.has(revision) and \
				sample_results.has(request_id):
			return true
		await process_frame
	return false


func _wait_for_snapshot(request_id: int) -> bool:
	for _frame in range(1800):
		if snapshot_results.has(request_id):
			return true
		if snapshot_failures.has(request_id):
			return false
		await process_frame
	return false


func _wait_for_snapshot_failure(request_id: int) -> bool:
	for _frame in range(900):
		if snapshot_failures.has(request_id):
			return true
		await process_frame
	return false


func _on_sample_ready(request_id: int, sample: RefCounted) -> void:
	sample_results[request_id] = sample


func _on_sample_failed(request_id: int, error: String) -> void:
	sample_failures[request_id] = error


func _on_sample_batch_ready(request_id: int, samples: Array) -> void:
	sample_batch_results[request_id] = samples


func _on_sample_batch_failed(request_id: int, error: String) -> void:
	sample_batch_failures[request_id] = error


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
	committed_revisions.push_back(world_revision)


func _remove_tree(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	var directory := DirAccess.open(path)
	if directory == null:
		return
	directory.list_dir_begin()
	var name := directory.get_next()
	while not name.is_empty():
		var child := path.path_join(name)
		if directory.current_is_dir():
			_remove_tree(child)
		else:
			DirAccess.remove_absolute(child)
		name = directory.get_next()
	directory.list_dir_end()
	DirAccess.remove_absolute(path)


func _fail(message: String) -> void:
	push_error("PRODUCTION_GODOT_SNAPSHOT_QUERY_FAIL: " + message)
	quit(1)
