#include "api/world_transvoxel_terrain.h"

#include "services/wt_chunk_application.h"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace world_transvoxel {
namespace {

std::filesystem::path query_globalized_path(const godot::String &path) {
	const godot::String global =
		godot::ProjectSettings::get_singleton()->globalize_path(path);
	const godot::CharString utf8 = global.utf8();
	return std::filesystem::u8path(utf8.get_data());
}

} // namespace

void WorldTransvoxelTerrain::bind_query_snapshot_methods() {
	godot::ClassDB::bind_method(
		godot::D_METHOD("request_authoritative_sample", "grid_point", "lod"),
		&WorldTransvoxelTerrain::request_authoritative_sample,
		DEFVAL(0)
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("request_authoritative_samples", "grid_points", "lod"),
		&WorldTransvoxelTerrain::request_authoritative_samples,
		DEFVAL(0)
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD(
			"request_world_compaction",
			"output_directory",
			"new_source_revision"
		),
		&WorldTransvoxelTerrain::request_world_compaction
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("request_world_migration", "output_directory"),
		&WorldTransvoxelTerrain::request_world_migration
	);
	ADD_SIGNAL(godot::MethodInfo(
		"authoritative_sample_ready",
		godot::PropertyInfo(godot::Variant::INT, "request_id"),
		godot::PropertyInfo(
			godot::Variant::OBJECT,
			"sample",
			godot::PROPERTY_HINT_RESOURCE_TYPE,
			"WorldTransvoxelSample"
		)
	));
	ADD_SIGNAL(godot::MethodInfo(
		"authoritative_sample_failed",
		godot::PropertyInfo(godot::Variant::INT, "request_id"),
		godot::PropertyInfo(godot::Variant::STRING, "error")
	));
	ADD_SIGNAL(godot::MethodInfo(
		"authoritative_samples_ready",
		godot::PropertyInfo(godot::Variant::INT, "request_id"),
		godot::PropertyInfo(godot::Variant::ARRAY, "samples")
	));
	ADD_SIGNAL(godot::MethodInfo(
		"authoritative_samples_failed",
		godot::PropertyInfo(godot::Variant::INT, "request_id"),
		godot::PropertyInfo(godot::Variant::STRING, "error")
	));
	ADD_SIGNAL(godot::MethodInfo(
		"world_snapshot_ready",
		godot::PropertyInfo(godot::Variant::INT, "request_id"),
		godot::PropertyInfo(godot::Variant::STRING, "manifest_path"),
		godot::PropertyInfo(godot::Variant::INT, "source_revision"),
		godot::PropertyInfo(godot::Variant::INT, "world_revision"),
		godot::PropertyInfo(godot::Variant::INT, "page_count")
	));
	ADD_SIGNAL(godot::MethodInfo(
		"world_snapshot_failed",
		godot::PropertyInfo(godot::Variant::INT, "request_id"),
		godot::PropertyInfo(godot::Variant::STRING, "error")
	));
}

godot::Ref<WorldTransvoxelChunkState>
WorldTransvoxelTerrain::query_chunk_state(
	const godot::Vector3i &chunk_coordinate,
	std::int64_t lod
) const {
	if (lod < 0 || lod > kWtMaximumLod) return {};
	const WtChunkKey key {
		chunk_coordinate.x,
		chunk_coordinate.y,
		chunk_coordinate.z,
		static_cast<std::uint8_t>(lod),
	};
	godot::Ref<WorldTransvoxelChunkState> snapshot;
	snapshot.instantiate();
	snapshot->set_snapshot(key, application_->find_record(key));
	return snapshot;
}

std::int64_t WorldTransvoxelTerrain::request_authoritative_sample(
	const godot::Vector3i &grid_point,
	std::int64_t lod
) {
	if (!lifecycle_ || lod < 0 || lod > kWtMaximumLod) {
		synchronous_world_error_ = "authoritative sample query is invalid";
		return 0;
	}
	std::uint64_t request_id = 0;
	const WtReadOnlyRuntimeStatus status =
		lifecycle_->request_authoritative_sample(
			{ grid_point.x, grid_point.y, grid_point.z },
			static_cast<std::uint8_t>(lod),
			request_id
		);
	synchronous_world_error_ = wt_read_only_runtime_status_message(status);
	return status == WtReadOnlyRuntimeStatus::Ok ?
		static_cast<std::int64_t>(request_id) : 0;
}

std::int64_t WorldTransvoxelTerrain::request_authoritative_samples(
	const godot::Array &grid_points,
	std::int64_t lod
) {
	if (!lifecycle_ || lod < 0 || lod > kWtMaximumLod ||
		grid_points.is_empty() ||
		static_cast<std::size_t>(grid_points.size()) >
			kWtMaximumAuthoritativeSampleBatchPoints) {
		synchronous_world_error_ = "authoritative sample query is invalid";
		return 0;
	}
	std::vector<WtGridPoint> points;
	points.reserve(static_cast<std::size_t>(grid_points.size()));
	for (std::int64_t index = 0; index < grid_points.size(); ++index) {
		const godot::Variant value = grid_points[index];
		if (value.get_type() != godot::Variant::VECTOR3I) {
			synchronous_world_error_ = "authoritative sample query is invalid";
			return 0;
		}
		const godot::Vector3i point = value;
		points.push_back({ point.x, point.y, point.z });
	}
	std::uint64_t request_id = 0;
	const WtReadOnlyRuntimeStatus status =
		lifecycle_->request_authoritative_samples(
			points,
			static_cast<std::uint8_t>(lod),
			request_id
		);
	synchronous_world_error_ = wt_read_only_runtime_status_message(status);
	return status == WtReadOnlyRuntimeStatus::Ok ?
		static_cast<std::int64_t>(request_id) : 0;
}

std::int64_t WorldTransvoxelTerrain::request_world_compaction(
	const godot::String &output_directory,
	std::int64_t new_source_revision
) {
	if (!lifecycle_ || output_directory.is_empty() ||
		new_source_revision <= get_world_source_revision()) {
		synchronous_world_error_ = "world compaction request is invalid";
		return 0;
	}
	std::uint64_t request_id = 0;
	const WtReadOnlyRuntimeStatus status = lifecycle_->request_world_snapshot(
		query_globalized_path(output_directory),
		static_cast<std::uint64_t>(new_source_revision),
		true,
		request_id
	);
	synchronous_world_error_ = wt_read_only_runtime_status_message(status);
	return status == WtReadOnlyRuntimeStatus::Ok ?
		static_cast<std::int64_t>(request_id) : 0;
}

std::int64_t WorldTransvoxelTerrain::request_world_migration(
	const godot::String &output_directory
) {
	if (!lifecycle_ || output_directory.is_empty()) {
		synchronous_world_error_ = "world migration request is invalid";
		return 0;
	}
	std::uint64_t request_id = 0;
	const WtReadOnlyRuntimeStatus status = lifecycle_->request_world_snapshot(
		query_globalized_path(output_directory),
		0,
		false,
		request_id
	);
	synchronous_world_error_ = wt_read_only_runtime_status_message(status);
	return status == WtReadOnlyRuntimeStatus::Ok ?
		static_cast<std::int64_t>(request_id) : 0;
}

} // namespace world_transvoxel
