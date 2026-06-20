#include "testing/wt_m3_integration_fixture.h"

#include "backend/wt_transvoxel_mit_backend.h"
#include "meshing/wt_chunk_mesher.h"
#include "physics/wt_collision_builder.h"
#include "physics/wt_godot_collision_sink.h"
#include "render/wt_godot_render_sink.h"
#include "services/wt_chunk_application.h"

namespace world_transvoxel {
namespace {

constexpr WtChunkKey kIntegrationChunkKey = { 0, 0, 0, 0 };

struct IntegrationSphereSource final : WtChunkSampleSource {
	bool sample(const WtGridPoint &point, WtScalarSample &output) const noexcept override {
		const double x = static_cast<double>(point.x - 8);
		const double y = static_cast<double>(point.y - 8);
		const double z = static_cast<double>(point.z - 8);
		const double density = x * x + y * y + z * z - 6.25 * 6.25;
		output.density = static_cast<float>(density);
		output.material = density < 0.0 ? 1 : 0;
		return true;
	}
};

std::shared_ptr<WtRenderPayload> make_payload(WtGenerationToken generation) {
	const WtChunkMesher mesher(wt_get_transvoxel_mit_backend());
	WtChunkMeshingScratch scratch;
	WtChunkMeshResult mesh;
	const IntegrationSphereSource source;
	if (mesher.mesh({ kIntegrationChunkKey, 0, 0.0F, 0.25F }, source, mesh, scratch) !=
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
	if (generation <= 0) {
		return false;
	}
	const WtGenerationToken token = { static_cast<std::uint64_t>(generation) };
	if (application.expect_chunk(kIntegrationChunkKey, token, collision_required) !=
		WtApplicationStatus::Ok) {
		return false;
	}
	auto render = make_payload(token);
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
	render_payload_ = render;
	return true;
}

bool WtM3IntegrationFixture::set_collision_distance(
	double distance,
	WtChunkApplicationService &application,
	WtGodotCollisionSink &collision_sink
) {
	const WtChunkApplicationRecord *record = application.find_record(kIntegrationChunkKey);
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
			application.set_collision_required(kIntegrationChunkKey, false);
			collision_sink.remove_collision(kIntegrationChunkKey);
		}
		return true;
	}
	if (record->collision_required) {
		return true;
	}
	if (application.set_collision_required(kIntegrationChunkKey, true) !=
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
	const WtChunkApplicationRecord *record = application.find_record(kIntegrationChunkKey);
	return record != nullptr && record->fully_ready();
}

std::int64_t WtM3IntegrationFixture::render_generation(
	const WtGodotRenderSink &sink
) const noexcept {
	return static_cast<std::int64_t>(sink.applied_generation(kIntegrationChunkKey).value);
}

std::int64_t WtM3IntegrationFixture::collision_generation(
	const WtGodotCollisionSink &sink
) const noexcept {
	return static_cast<std::int64_t>(sink.applied_generation(kIntegrationChunkKey).value);
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
	render_sink.remove_render(kIntegrationChunkKey);
	collision_sink.remove_collision(kIntegrationChunkKey);
	application.forget_chunk(kIntegrationChunkKey);
	render_payload_.reset();
}

} // namespace world_transvoxel
