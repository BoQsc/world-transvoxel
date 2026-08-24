#include "diagnostics/wt_gpu_meshing_shadow.h"

#include <cstdlib>
#include <iostream>

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

void require(bool condition, const char *message) {
	if (condition) return;
	std::cerr << "GPU_MESHING_SHADOW_TEST_FAIL: " << message << '\n';
	std::exit(1);
}

} // namespace

int main() {
	WtGpuMeshingShadowQueue queue;
	require(!queue.begin(0), "zero capacity was accepted");
	require(queue.begin(1), "bounded queue did not start");
	require(queue.capture(capture_for(11)), "first capture failed");
	WtGpuMeshingShadowRequest first;
	require(queue.pop(first), "first request did not pop");
	require(first.records.size() == 1, "popped request lost its cell records");
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
	queue.end();
	require(!queue.metrics().enabled, "queue remained enabled after end");

	std::cout << "GPU_MESHING_SHADOW_TEST_PASS capacity=2 stale=1 mismatched=1\n";
	return 0;
}
