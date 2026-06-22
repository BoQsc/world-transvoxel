class_name WorldTransvoxelExampleViewer
extends Node3D

signal viewer_changed(position: Vector3)


func _ready() -> void:
	set_notify_transform(true)
	viewer_changed.emit(global_position)


func _notification(what: int) -> void:
	if what == NOTIFICATION_TRANSFORM_CHANGED and is_inside_tree():
		viewer_changed.emit(global_position)
