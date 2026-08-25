#include "diagnostics/wt_gpu_meshing_shadow.h"
#include "diagnostics/wt_gpu_meshing_input_pack.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using namespace world_transvoxel;

WtGpuMeshingShadowCapture capture_for(
	std::uint64_t generation,
	std::uint64_t world_revision = 0
) {
	WtGpuMeshingShadowCapture capture;
	capture.job.key = { 1, 2, 3, 0 };
	capture.job.generation.value = generation;
	capture.job.source_revision = 7;
	capture.job.world_revision = world_revision;
	capture.transition_mask = 3;
	capture.cached_transition_mask = 1;
	capture.static_water_surface_expected = true;
	capture.records.resize(1);
	return capture;
}

WtGpuMeshingShadowIdentity identity_for(
	const WtGpuMeshingShadowRequest &request
) {
	WtGpuMeshingShadowIdentity identity;
	identity.key = request.job.key;
	identity.generation = request.job.generation;
	identity.source_revision = request.job.source_revision;
	identity.world_revision = request.job.world_revision;
	identity.transition_mask = request.transition_mask;
	identity.surface = request.surface;
	return identity;
}

WtCellSample sample_for(float density, std::uint16_t material) {
	WtCellSample sample;
	sample.density = density;
	sample.gradient = { 1.0F, 2.0F, 3.0F };
	sample.material = material;
	sample.material_authored = material % 2U != 0U;
	return sample;
}

WtGpuMeshingShadowRequest packing_request() {
	WtGpuMeshingShadowRequest request;
	request.job.key = { -2, 3, 4, 2 };
	request.job.generation.value = 0x80000011ULL;
	request.job.source_revision = 0x1234567887654321ULL;
	request.job.world_revision = 0xfedcba9876543210ULL;
	request.transition_mask = 37;
	request.surface = WtGpuMeshingShadowSurface::StaticWater;
	WtRecordedMeshingCell regular;
	regular.type = WtRecordedCellType::Regular;
	regular.regular_input.origin = { 10.0F, 20.0F, 30.0F };
	regular.regular_input.cell_size = 4.0F;
	regular.regular_input.isovalue = 0.25F;
	for (unsigned int index = 0; index < kWtRegularSampleCount; ++index) {
		regular.regular_input.samples[index] = sample_for(
			static_cast<float>(index) - 3.5F,
			static_cast<std::uint16_t>(index + 1U)
		);
	}
	WtRecordedMeshingCell transition;
	transition.type = WtRecordedCellType::Transition;
	transition.transition_input.full_resolution_origin = { -1.0F, 2.0F, 8.0F };
	transition.transition_input.sample_spacing = 2.0F;
	transition.transition_input.transition_width = 0.5F;
	transition.transition_input.isovalue = -0.25F;
	transition.transition_input.orientation = WtTransitionOrientation::NegativeZ;
	for (unsigned int index = 0; index < kWtTransitionSampleCount; ++index) {
		transition.transition_input.samples[index] = sample_for(
			6.0F - static_cast<float>(index),
			static_cast<std::uint16_t>(index + 20U)
		);
	}
	request.records = { regular, transition };
	return request;
}

void require(bool condition, const char *message) {
	if (condition) return;
	std::cerr << "GPU_MESHING_SHADOW_TEST_FAIL: " << message << '\n';
	std::exit(1);
}

} // namespace

int main() {
	WtGpuMeshingInputPack packed;
	std::string packing_error;
	require(
		wt_pack_gpu_meshing_input(packing_request(), packed, packing_error),
		"valid resident request did not pack"
	);
	require(
		packed.cell_count == 2 && packed.sample_count == 17,
		"packed resident dimensions changed"
	);
	require(
		packed.field_values.size() == 68 && packed.field_meta.size() == 68 &&
			packed.cell_headers.size() == 8 && packed.cell_origins.size() == 8 &&
			packed.cell_options.size() == 8 &&
			packed.sample_references.size() == 17,
		"packed resident buffer sizes changed"
	);
	require(
		packed.cell_headers[0] == 0 && packed.cell_headers[2] == 0 &&
			packed.cell_headers[3] == 8 && packed.cell_headers[4] == 1 &&
			packed.cell_headers[5] == 5 && packed.cell_headers[6] == 8 &&
			packed.cell_headers[7] == 9,
		"packed resident cell headers changed"
	);
	require(
		packed.field_values[0] == -3.5F && packed.field_values[1] == 1.0F &&
			packed.field_meta[0] == 1 && packed.field_meta[1] == 1 &&
			packed.field_meta[2] == 0 && packed.sample_references[16] == 16,
		"packed resident sample layout changed"
	);
	require(
		packed.config[0] == 2 && packed.config[1] == 17 &&
			packed.config[4] == -2 && packed.config[7] == 2 &&
			packed.config[13] == 37 && packed.config[14] == 1 &&
			packed.config[15] == 17,
		"packed resident identity layout changed"
	);
	require(packed.packed_byte_count > 50000, "packed byte metric is incomplete");
	WtGpuMeshingShadowRequest invalid_request = packing_request();
	invalid_request.records[0].regular_input.samples[0].density =
		std::numeric_limits<float>::infinity();
	require(
		!wt_pack_gpu_meshing_input(invalid_request, packed, packing_error),
		"non-finite resident sample was accepted"
	);

	WtGpuMeshingShadowQueue queue;
	require(!queue.begin(0), "zero capacity was accepted");
	require(queue.begin(1), "bounded queue did not start");
	require(queue.capture(capture_for(11)), "first capture failed");
	WtGpuMeshingShadowRequest first;
	require(queue.pop(first), "first request did not pop");
	require(first.records.size() == 1, "popped request lost its cell records");
	require(
		first.static_water_surface_expected,
		"popped request lost its expected surface inventory"
	);
	require(
		!queue.capture(capture_for(12)),
		"in-flight request was not counted against capacity"
	);
	const WtGpuMeshingShadowCompletion matched = queue.complete(
		first.request_id, identity_for(first), 7, 0, true, {}
	);
	require(
		matched.status == WtGpuMeshingShadowCompletionStatus::Matched,
		"matching completion was rejected"
	);
	require(
		matched.has_retained_request &&
			matched.retained_request.job.generation == first.job.generation,
		"matching completion lost its retained publication identity"
	);

	require(queue.begin(2), "freshness queue restart failed");
	require(queue.capture(capture_for(15, 0)), "freshness in-flight capture failed");
	WtGpuMeshingShadowRequest freshness_in_flight;
	require(queue.pop(freshness_in_flight), "freshness request did not enter flight");
	require(queue.capture(capture_for(16, 0)), "freshness queued capture failed");
	require(
		queue.capture(capture_for(17, 1)),
		"newer world revision did not replace queued validation work"
	);
	WtGpuMeshingShadowRequest freshest_request;
	require(queue.pop(freshest_request), "freshest queued request did not pop");
	require(
		freshest_request.job.world_revision == 1 &&
			freshest_request.job.generation.value == 17,
		"freshness replacement retained the wrong request"
	);
	const WtGpuMeshingShadowCompletion superseded_in_flight = queue.complete(
		freshness_in_flight.request_id,
		identity_for(freshness_in_flight),
		7,
		1,
		true,
		{}
	);
	require(
		superseded_in_flight.status == WtGpuMeshingShadowCompletionStatus::Stale,
		"older in-flight revision was not stale"
	);
	const WtGpuMeshingShadowCompletion freshest_completion = queue.complete(
		freshest_request.request_id,
		identity_for(freshest_request),
		7,
		1,
		true,
		{}
	);
	require(
		freshest_completion.status == WtGpuMeshingShadowCompletionStatus::Matched,
		"freshness replacement did not complete"
	);
	const WtGpuMeshingShadowMetrics freshness_metrics = queue.metrics();
	require(
		freshness_metrics.superseded_queued_requests == 1,
		"queued supersession metric was not recorded"
	);

	require(queue.begin(2), "queue restart failed");
	require(queue.capture(capture_for(20)), "identity capture failed");
	WtGpuMeshingShadowRequest identity_request;
	require(queue.pop(identity_request), "identity request did not pop");
	WtGpuMeshingShadowIdentity wrong_identity = identity_for(identity_request);
	wrong_identity.transition_mask ^= 1;
	const WtGpuMeshingShadowCompletion identity_mismatch = queue.complete(
		identity_request.request_id, wrong_identity, 7, 0, true, {}
	);
	require(
		identity_mismatch.status ==
			WtGpuMeshingShadowCompletionStatus::IdentityMismatch,
		"changed identity was accepted"
	);

	require(queue.capture(capture_for(30)), "stale revision capture failed");
	WtGpuMeshingShadowRequest stale_revision_request;
	require(queue.pop(stale_revision_request), "stale revision request did not pop");
	const WtGpuMeshingShadowCompletion stale_revision = queue.complete(
		stale_revision_request.request_id,
		identity_for(stale_revision_request),
		7,
		1,
		true,
		{}
	);
	require(
		stale_revision.status == WtGpuMeshingShadowCompletionStatus::Stale,
		"changed world revision was not stale"
	);

	require(queue.begin(2), "latest-generation queue restart failed");
	require(queue.capture(capture_for(40)), "older generation capture failed");
	require(queue.capture(capture_for(41)), "newer generation capture failed");
	WtGpuMeshingShadowRequest older_request;
	require(queue.pop(older_request), "older generation request did not pop");
	const WtGpuMeshingShadowCompletion stale_generation = queue.complete(
		older_request.request_id,
		identity_for(older_request),
		7,
		0,
		true,
		{}
	);
	require(
		stale_generation.status == WtGpuMeshingShadowCompletionStatus::Stale,
		"superseded generation was not stale"
	);
	WtGpuMeshingShadowRequest newer_request;
	require(queue.pop(newer_request), "newer generation request did not pop");
	const WtGpuMeshingShadowCompletion mismatched = queue.complete(
		newer_request.request_id,
		identity_for(newer_request),
		7,
		0,
		false,
		"geometry differs"
	);
	require(
		mismatched.status == WtGpuMeshingShadowCompletionStatus::Mismatched,
		"GPU mismatch was not retained"
	);
	const WtGpuMeshingShadowMetrics metrics = queue.metrics();
	require(metrics.stale_results == 1, "stale metric changed after restart");
	require(metrics.mismatched_results == 1, "mismatch metric was not recorded");
	require(metrics.in_flight_requests == 0, "completed request remained in flight");

	require(queue.begin(2), "resident validation queue restart failed");
	require(queue.capture(capture_for(50, 4)), "resident capture failed");
	WtGpuMeshingShadowRequest resident_request;
	require(queue.pop(resident_request), "resident request did not pop");
	const WtGpuMeshingResidentValidation resident_ready =
		queue.validate_resident(
			resident_request.request_id,
			identity_for(resident_request),
			7,
			4
		);
	require(
		resident_ready.status == WtGpuMeshingResidentValidationStatus::Ready,
		"current resident request was not admitted"
	);
	require(queue.capture(capture_for(51, 4)), "resident rejection capture failed");
	WtGpuMeshingShadowRequest rejected_request;
	require(queue.pop(rejected_request), "resident rejection did not pop");
	const WtGpuMeshingResidentValidation resident_rejected =
		queue.reject_resident(
			rejected_request.request_id,
			identity_for(rejected_request),
			"render allocation failed"
		);
	require(
		resident_rejected.status ==
			WtGpuMeshingResidentValidationStatus::Rejected,
		"resident rejection was not retained"
	);
	const WtGpuMeshingShadowMetrics resident_metrics = queue.metrics();
	require(
		resident_metrics.resident_ready_results == 1 &&
			resident_metrics.resident_rejected_results == 1 &&
			resident_metrics.matched_results == 0,
		"resident validation polluted differential result metrics"
	);
	queue.end();
	require(!queue.metrics().enabled, "queue remained enabled after end");

	std::cout << "GPU_MESHING_SHADOW_TEST_PASS capacity=2 stale=1 mismatched=1 "
		"queued_superseded=1\n";
	return 0;
}
