extends SceneTree


const RENDER_COUNT := 32
const COLLISION_COUNT := 16
const RENDER_BUDGET := 4
const COLLISION_BUDGET := 2
const FRAMES_PER_RUN := 8
const MAX_RENDER_RESOURCE_COUNT := 160


func _initialize() -> void:
	call_deferred("_run_benchmark")


func _parse_count(arguments: PackedStringArray, name: String, fallback: int) -> int:
	for index in range(arguments.size() - 1):
		if arguments[index] == name:
			return int(arguments[index + 1])
	return fallback


func _run_scenario(terrain: Node, generation: int) -> Dictionary:
	if not terrain.call(
			"_m5_benchmark_prepare_batch",
			RENDER_COUNT,
			COLLISION_COUNT,
			generation
	):
		return {"valid": false, "message": "batch preparation failed"}
	var result := {
		"valid": true,
		"scenario_ns": 0,
		"frame_max_ns": 0,
		"render_sink_ns": 0,
		"collision_sink_ns": 0,
	}
	for frame in range(FRAMES_PER_RUN):
		var sample: Dictionary = terrain.call("_m5_benchmark_apply_frame")
		if not sample.get("valid", false):
			return {"valid": false, "message": "application frame was invalid"}
		var expected_render: int = min(
			RENDER_BUDGET,
			RENDER_COUNT - frame * RENDER_BUDGET
		)
		var expected_collision: int = min(
			COLLISION_BUDGET,
			COLLISION_COUNT - frame * COLLISION_BUDGET
		)
		if sample.get("render_processed", -1) != expected_render or \
				sample.get("collision_processed", -1) != expected_collision:
			return {"valid": false, "message": "frame budget mismatch"}
		var duration_ns: int = sample["duration_ns"]
		result["scenario_ns"] += duration_ns
		result["frame_max_ns"] = max(result["frame_max_ns"], duration_ns)
		result["render_sink_ns"] += sample["render_sink_ns"]
		result["collision_sink_ns"] += sample["collision_sink_ns"]
		if frame == FRAMES_PER_RUN - 1 and \
				sample.get("ready_count", 0) != RENDER_COUNT:
			return {"valid": false, "message": "final readiness mismatch"}
		await process_frame
	if terrain.get_queued_render_count() != 0 or \
			terrain.get_queued_collision_count() != 0:
		return {"valid": false, "message": "application queues did not drain"}
	var render_resources: int = terrain.get_render_resource_count()
	var collision_resources: int = terrain.get_collision_resource_count()
	if render_resources < RENDER_COUNT or \
			render_resources > MAX_RENDER_RESOURCE_COUNT or \
			collision_resources != COLLISION_COUNT:
		return {
			"valid": false,
			"message": "resource count mismatch render=%d collision=%d" % [
				render_resources,
				collision_resources,
			],
		}
	if terrain.get_render_latency_frames_maximum() != FRAMES_PER_RUN or \
			terrain.get_collision_latency_frames_maximum() != FRAMES_PER_RUN:
		return {"valid": false, "message": "readiness latency mismatch"}
	return result


func _run_benchmark() -> void:
	var arguments := OS.get_cmdline_user_args()
	var iterations := _parse_count(arguments, "--iterations", 11)
	var warmup := _parse_count(arguments, "--warmup", 2)
	if iterations < 3 or warmup < 1:
		return _fail("iterations and warmup are below the contract minimum")

	var terrain := WorldTransvoxelTerrain.new()
	root.add_child(terrain)
	if terrain.get_render_apply_budget() != RENDER_BUDGET or \
			terrain.get_collision_apply_budget() != COLLISION_BUDGET:
		return _fail("production default application budgets changed")
	terrain.set_process(false)
	terrain.set_render_apply_budget(0)
	terrain.set_collision_apply_budget(0)
	await process_frame
	if terrain.get_render_apply_budget() != 0 or \
			terrain.get_collision_apply_budget() != 0:
		return _fail("zero automatic application budget was not retained")
	var generation := 1
	var cold: Dictionary = await _run_scenario(terrain, generation)
	if not cold.get("valid", false):
		return _fail("cold run: " + cold.get("message", "unknown failure"))
	print(
		"M5_GODOT_APPLICATION_COLD duration_ns=%d frame_max_ns=%d" % [
			cold["scenario_ns"],
			cold["frame_max_ns"],
		]
	)

	for warmup_index in range(warmup):
		generation += 1
		var warmup_result: Dictionary = await _run_scenario(terrain, generation)
		if not warmup_result.get("valid", false):
			return _fail(
				"warmup %d: %s" % [
					warmup_index,
					warmup_result.get("message", "unknown failure"),
				]
			)

	for run in range(iterations):
		generation += 1
		var sample: Dictionary = await _run_scenario(terrain, generation)
		if not sample.get("valid", false):
			return _fail(
				"sample %d: %s" % [
					run,
					sample.get("message", "unknown failure"),
				]
			)
		print(
			(
				"M5_GODOT_APPLICATION_SAMPLE run=%d scenario_ns=%d "
				+ "frame_max_ns=%d render_sink_ns=%d collision_sink_ns=%d"
			) % [
				run,
				sample["scenario_ns"],
				sample["frame_max_ns"],
				sample["render_sink_ns"],
				sample["collision_sink_ns"],
			]
		)

	var clear_ns: int = terrain.call("_m5_benchmark_clear")
	if clear_ns <= 0 or terrain.get_render_resource_count() != 0 or \
			terrain.get_collision_resource_count() != 0:
		return _fail("resource teardown failed")
	terrain.queue_free()
	await process_frame
	print("M5_GODOT_APPLICATION_CLEAR duration_ns=%d" % clear_ns)
	print(
		(
			"M5_GODOT_APPLICATION_PASS runs=%d warmup=%d frames_per_run=%d "
			+ "render_count=%d collision_count=%d render_budget=%d "
			+ "collision_budget=%d readiness_max=%d"
		) % [
			iterations,
			warmup,
			FRAMES_PER_RUN,
			RENDER_COUNT,
			COLLISION_COUNT,
			RENDER_BUDGET,
			COLLISION_BUDGET,
			FRAMES_PER_RUN,
		]
	)
	quit(0)


func _fail(message: String) -> void:
	push_error("M5_GODOT_APPLICATION_FAIL: " + message)
	quit(1)
