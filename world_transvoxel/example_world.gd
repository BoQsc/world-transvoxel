class_name WorldTransvoxelExampleWorld
extends Node3D

signal example_ready
signal example_failed(message: String)

@export_file("*.wtworld") var world_manifest_path := \
	"res://build/world-transvoxel-example/world.wtworld"
@export_dir var object_root_path := "res://build/world-transvoxel-example"
@export var autostart := true
@export_range(1, 1024, 1) var viewer_id := 1
@export_range(0, 64, 1) var radius_chunks := 1
@export_range(0, 20, 1) var maximum_lod := 1

@onready var terrain: Node3D = $Terrain
@onready var viewer: Node3D = $Viewer

var _viewer_revision := 0
var _pending_position := Vector3.ZERO
var _last_submitted_position := Vector3.ZERO
var _has_submitted_position := false


func _ready() -> void:
	_pending_position = viewer.global_position
	viewer.viewer_changed.connect(_on_viewer_changed)
	terrain.connect("world_state_changed", _on_world_state_changed)
	terrain.connect("world_failed", _on_world_failed)
	if autostart and not start_example():
		_fail(terrain.call("get_world_error"))


func _exit_tree() -> void:
	if terrain != null and terrain.call("is_world_running"):
		terrain.call("stop_world")


func start_example() -> bool:
	return terrain.call(
		"start_world", world_manifest_path, object_root_path
	)


func stop_example() -> bool:
	return terrain.call("stop_world")


func _on_world_state_changed(_state: int, state_name: String) -> void:
	if state_name != "running":
		return
	if _submit_viewer():
		example_ready.emit()


func _on_world_failed(message: String) -> void:
	_fail(message)


func _on_viewer_changed(position: Vector3) -> void:
	_pending_position = position
	if terrain.call("is_world_running"):
		_submit_viewer()


func _submit_viewer() -> bool:
	if _has_submitted_position and \
			_pending_position.is_equal_approx(_last_submitted_position):
		return true
	_viewer_revision += 1
	if not terrain.call(
		"update_viewer",
		viewer_id,
		_viewer_revision,
		_pending_position,
		radius_chunks,
		maximum_lod
	):
		_fail(terrain.call("get_world_error"))
		return false
	_last_submitted_position = _pending_position
	_has_submitted_position = true
	return true


func _fail(message: String) -> void:
	example_failed.emit(message)
	push_error("World Transvoxel example failed: " + message)
