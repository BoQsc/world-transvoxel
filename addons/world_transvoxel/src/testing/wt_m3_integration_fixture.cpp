#include "testing/wt_m3_integration_fixture.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "meshing/wt_chunk_mesher.h"
#include "physics/wt_collision_builder.h"
#include "physics/wt_godot_collision_sink.h"
#include "render/wt_godot_render_sink.h"
#include "services/wt_chunk_application.h"

#include <limits>

namespace world_transvoxel {
namespace {

constexpr WtChunkKey kIntegrationChunkKey = { 0, 0, 0, 0 };

struct IntegrationSphereSource final : WtChunkSampleSource {
	WtGridPoint center;
	double radius = 0.0;

	bool sample(const WtGridPoint &point, WtScalarSample &output) const noexcept override {
		const double x = static_cast<double>(point.x - center.x);
		const double y = static_cast<double>(point.y - center.y);
		const double z = static_cast<double>(point.z - center.z);
		const double density = x * x + y * y + z * z - radius * radius;
		output.density = static_cast<float>(density);
		output.material = density < 0.0 ? 1 : 0;
		return true;
	}
};

bool to_chunk_coordinate(std::int64_t value, std::int32_t &output) noexcept {
	if (value < std::numeric_limits<std::int32_t>::min() ||
		value > std::numeric_limits<std::int32_t>::max()) {
		return false;
	}
	output = static_cast<std::int32_t>(value);
	return true;
}

std::shared_ptr<WtRenderPayload> make_payload(
	const WtChunkKey &key,
	WtGenerationToken generation
) {
	const WtChunkMesher mesher(wt_get_transvoxel_mit_backend());
	WtChunkMeshingScratch scratch;
	WtChunkMeshResult mesh;
	const WtChunkBounds bounds = wt_chunk_bounds(key);
	const std::int64_t half_extent = wt_chunk_extent(key.lod) / 2;
	IntegrationSphereSource source;
	source.center = {
		bounds.minimum.x + half_extent,
		bounds.minimum.y + half_extent,
		bounds.minimum.z + half_extent,
	};
	source.radius = 6.25 * static_cast<double>(wt_lod_cell_size(key.lod));
	if (mesher.mesh({ key, 0, 0.0F, 0.25F }, source, mesh, scratch) !=
		WtChunkMeshingStatus::Ok) {
		return {};
	}
	auto payload = std::make_shared<WtRenderPayload>();
	return wt_build_render_payload(mesh, generation, *payload) == WtRenderBuildStatus::Ok ?
		payload : std::shared_ptr<WtRenderPayload>{};
}

} // namespace

bool WtM3IntegrationFixture::submit_generation(
	std::int64_t generation,
	bool collision_required,
	WtChunkApplicationService &application
) {
	return submit_generation_for_key(
		kIntegrationChunkKey, generation, collision_required, application
	);
}

bool WtM3IntegrationFixture::submit_chunk_generation(
	std::int64_t chunk_x,
	std::int64_t chunk_y,
	std::int64_t chunk_z,
	std::int64_t generation,
	bool collision_required,
	WtChunkApplicationService &application
) {
	WtChunkKey key;
	if (!to_chunk_coordinate(chunk_x, key.x) ||
		!to_chunk_coordinate(chunk_y, key.y) ||
		!to_chunk_coordinate(chunk_z, key.z)) {
		return false;
	}
	return submit_generation_for_key(key, generation, collision_required, application);
}

bool WtM3IntegrationFixture::submit_generation_for_key(
	const WtChunkKey &key,
	std::int64_t generation,
	bool collision_required,
	WtChunkApplicationService &application
) {
	if (generation <= 0) {
		return false;
	}
	const WtGenerationToken token = { static_cast<std::uint64_t>(generation) };
	if (application.expect_chunk(key, token, collision_required) !=
		WtApplicationStatus::Ok) {
		return false;
	}
	auto render = make_payload(key, token);
	if (!render || application.submit_render(render) != WtApplicationStatus::Ok) {
		return false;
	}
	if (collision_required) {
		auto collision = std::make_shared<WtCollisionPayload>();
		if (wt_build_collision_payload(*render, {}, *collision) !=
			WtCollisionBuildStatus::Ok ||
			application.submit_collision(collision) != WtApplicationStatus::Ok) {
			return false;
		}
	}
	active_key_ = key;
	render_payload_ = render;
	return true;
}

bool WtM3IntegrationFixture::set_collision_distance(
	double distance,
	WtChunkApplicationService &application,
	WtGodotCollisionSink &collision_sink
) {
	const WtChunkApplicationRecord *record = application.find_record(active_key_);
	if (record == nullptr || !render_payload_) {
		return false;
	}
	const WtCollisionRequirement requirement = wt_evaluate_collision_requirement(
		{}, record->collision_required, distance
	);
	if (requirement == WtCollisionRequirement::Invalid) {
		return false;
	}
	if (requirement == WtCollisionRequirement::NotRequired) {
		if (record->collision_required) {
			application.set_collision_required(active_key_, false);
			collision_sink.remove_collision(active_key_);
		}
		return true;
	}
	if (record->collision_required) {
		return true;
	}
	if (application.set_collision_required(active_key_, true) !=
		WtApplicationStatus::Ok) {
		return false;
	}
	auto collision = std::make_shared<WtCollisionPayload>();
	return wt_build_collision_payload(*render_payload_, {}, *collision) ==
		WtCollisionBuildStatus::Ok &&
		application.submit_collision(collision) == WtApplicationStatus::Ok;
}

bool WtM3IntegrationFixture::fully_ready(
	const WtChunkApplicationService &application
) const noexcept {
	const WtChunkApplicationRecord *record = application.find_record(active_key_);
	return record != nullptr && record->fully_ready();
}

std::int64_t WtM3IntegrationFixture::render_generation(
	const WtGodotRenderSink &sink
) const noexcept {
	return static_cast<std::int64_t>(sink.applied_generation(active_key_).value);
}

std::int64_t WtM3IntegrationFixture::collision_generation(
	const WtGodotCollisionSink &sink
) const noexcept {
	return static_cast<std::int64_t>(sink.applied_generation(active_key_).value);
}

std::int64_t WtM3IntegrationFixture::stale_render_count(
	const WtChunkApplicationService &application
) const noexcept {
	return static_cast<std::int64_t>(application.get_metrics().stale_render);
}

std::int64_t WtM3IntegrationFixture::stale_collision_count(
	const WtChunkApplicationService &application
) const noexcept {
	return static_cast<std::int64_t>(application.get_metrics().stale_collision);
}

void WtM3IntegrationFixture::forget(
	WtChunkApplicationService &application,
	WtGodotRenderSink &render_sink,
	WtGodotCollisionSink &collision_sink
) {
	render_sink.remove_render(active_key_);
	collision_sink.remove_collision(active_key_);
	application.forget_chunk(active_key_);
	render_payload_.reset();
}

} // namespace world_transvoxel
