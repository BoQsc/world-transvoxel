extends RefCounted


static func configure(config: Resource) -> void:
	config.set("active_chunk_capacity", 40)
	config.set("viewer_capacity", 2)
	config.set("demand_capacity_per_viewer", 125)
	config.set("storage_request_capacity", 64)
	config.set("storage_completion_capacity", 64)
	config.set("encoded_page_entry_capacity", 40)
	config.set("decoded_page_entry_capacity", 40)
	config.set("mesh_entry_capacity", 40)
	config.set("render_entry_capacity", 40)
	config.set("collision_entry_capacity", 40)
	config.set("render_apply_budget", 4)
	config.set("collision_apply_budget", 2)


static func duration_argument(default_duration_ms: int) -> int:
	var arguments := OS.get_cmdline_user_args()
	for index in range(arguments.size() - 1):
		if arguments[index] == "--duration-ms":
			var value := int(arguments[index + 1])
			if value >= 1000 and value <= 300000:
				return value
	return default_duration_ms


static func percentile_frame_us(samples: Array[int], fraction: float) -> int:
	if samples.is_empty():
		return 0
	var sorted := samples.duplicate()
	sorted.sort()
	var index := clampi(
		int(ceil(float(sorted.size()) * fraction)) - 1,
		0,
		sorted.size() - 1
	)
	return sorted[index]


static func remove_tree(path: String) -> void:
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
			remove_tree(child)
		else:
			DirAccess.remove_absolute(child)
		name = directory.get_next()
	directory.list_dir_end()
	DirAccess.remove_absolute(path)
