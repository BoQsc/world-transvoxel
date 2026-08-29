#include "diagnostics/wt_gpu_meshing_shadow.h"
#include "diagnostics/wt_gpu_meshing_input_pack.h"
#include "backend/wt_transvoxel_mit_backend.h"
#include "storage/wt_chunk_page.h"

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

WtGpuMeshingShadowRequest page_field_request() {
	WtGpuMeshingShadowRequest request;
	request.job.key = { 2, -1, 3, 1 };
	request.job.generation.value = 19;
	request.job.source_revision = 7;
	request.job.world_revision = 11;
	request.transition_mask = 5;
	request.cached_transition_mask = 5;
	request.capture_stage = WtGpuMeshingCaptureStage::PreMeshField;
	auto page = std::make_shared<WtChunkPage>();
	page->metadata.key = request.job.key;
	page->metadata.cell_spacing = 2;
	page->metadata.source_revision = request.job.source_revision;
	page->samples.resize(kWtChunkPageSampleCount);
	for (std::size_t index = 0; index < page->samples.size(); ++index) {
		page->samples[index].density = static_cast<float>(index % 37U) - 18.0F;
		page->samples[index].material = static_cast<std::uint16_t>(index % 9U);
		page->samples[index].material_authored = (index % 3U) == 0;
	}
	WtChunkSurfaceShiftRecord shift;
	shift.edge_index = 0;
	shift.unit_offset = 0;
	shift.sample_a = sample_for(-0.5F, 3);
	shift.sample_b = sample_for(0.5F, 4);
	page->surface_shift_records.push_back(shift);
	request.retained_pages.push_back({ request.job.key, std::move(page) });
	return request;
}

void require(bool condition, const char *message) {
	if (condition) return;
	std::cerr << "GPU_MESHING_SHADOW_TEST_FAIL: " << message << '\n';
	std::exit(1);
}

} // namespace

int main() {
	WtTransvoxelMitBackend topology_authority;
	WtFieldCaptureMeshingBackend field_capture(topology_authority);
	const WtGpuMeshingShadowRequest field_fixture = packing_request();
	WtCellMesh ignored_cell_mesh;
	WtCellMeshingScratch ignored_cell_scratch;
	require(
		field_capture.mesh_regular_cell(
			field_fixture.records[0].regular_input,
			ignored_cell_mesh,
			ignored_cell_scratch
		) == WtCellStatus::Empty,
		"pre-mesh regular field capture attempted CPU topology"
	);
	require(
		field_capture.mesh_transition_cell(
			field_fixture.records[1].transition_input,
			ignored_cell_mesh,
			ignored_cell_scratch
		) == WtCellStatus::Empty,
		"pre-mesh transition field capture attempted CPU topology"
	);
	require(
		field_capture.take_records().size() == 2 &&
			field_capture.cpu_topology_call_count() == 0 &&
			!field_capture.overflowed(),
		"pre-mesh field capture did not retain exact bounded inputs"
	);

	WtGpuMeshingInputPack packed;
	std::string packing_error;
	require(
		wt_pack_gpu_meshing_input(packing_request(), packed, packing_error),
		"valid resident request did not pack"
	);
	require(
		packed.cell_count == 2 && packed.sample_count == 17 &&
			!packed.proven_empty,
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
		packed.cell_origins[0] == -118.0F &&
			packed.cell_origins[1] == 212.0F &&
			packed.cell_origins[2] == 286.0F &&
			packed.cell_origins[4] == -129.0F &&
			packed.cell_origins[5] == 194.0F &&
			packed.cell_origins[6] == 264.0F,
		"packed resident cell origins are not world-relative"
	);
	require(
		packed.config[0] == 2 && packed.config[1] == 17 &&
			packed.config[4] == -2 && packed.config[7] == 2 &&
			packed.config[13] == 37 && packed.config[14] == 1 &&
			packed.config[15] == 17,
		"packed resident identity layout changed"
	);
	require(
		packed.bounds_min.x == -129.0F && packed.bounds_min.y == 190.0F &&
			packed.bounds_min.z == 263.5F && packed.bounds_max.x == -114.0F &&
			packed.bounds_max.y == 216.0F && packed.bounds_max.z == 290.0F,
		"packed resident bounds changed"
	);
	require(packed.packed_byte_count > 50000, "packed byte metric is incomplete");
	WtGpuMeshingInputPack page_packed;
	require(
		wt_pack_gpu_meshing_input(
			page_field_request(), page_packed, packing_error
		),
		"valid page-field resident request did not pack"
	);
	require(
		page_packed.page_field_input &&
			page_packed.cell_count == 4608 &&
			page_packed.sample_count == kWtChunkPageSampleCount + 2U &&
			!page_packed.proven_empty &&
			page_packed.config[2] == 1 && page_packed.config[3] == 1 &&
			page_packed.config[15] == 5,
		"page-field resident dimensions or mode changed"
	);
	WtGpuMeshingShadowRequest empty_page_request = page_field_request();
	auto empty_page = std::make_shared<WtChunkPage>(
		*empty_page_request.retained_pages[0].page
	);
	empty_page->surface_shift_records.clear();
	for (WtScalarSample &sample : empty_page->samples) {
		sample.density = 1.0F;
	}
	empty_page_request.retained_pages[0].page = std::move(empty_page);
	WtGpuMeshingInputPack empty_page_packed;
	require(
		wt_pack_gpu_meshing_input(
			empty_page_request, empty_page_packed, packing_error
		) && empty_page_packed.proven_empty,
		"uniform signed page field was not proven empty"
	);
	require(
		page_packed.field_values.size() == (kWtChunkPageSampleCount + 2U) * 4U &&
			page_packed.field_meta.size() == (kWtChunkPageSampleCount + 2U) * 4U &&
			page_packed.cell_headers.size() == 4 &&
			page_packed.cell_origins.size() == 4 &&
			page_packed.cell_options.size() == 4 &&
			page_packed.sample_references.size() == 5,
		"page-field resident buffer inventory changed"
	);
	require(
		page_packed.cell_headers[0] == 1 && page_packed.cell_headers[1] == 1 &&
			page_packed.cell_headers[2] == 0 && page_packed.cell_headers[3] == 1 &&
			page_packed.sample_references[1] == 0 &&
			page_packed.sample_references[2] == 0 &&
			page_packed.sample_references[3] ==
				static_cast<std::int32_t>(kWtChunkPageSampleCount) &&
			page_packed.sample_references[4] ==
				static_cast<std::int32_t>(kWtChunkPageSampleCount + 1U) &&
			page_packed.field_values[kWtChunkPageSampleCount * 4U] == -0.5F,
		"page-field surface-shift authority layout changed"
	);
	require(
		page_packed.bounds_min.x == 64.0F &&
			page_packed.bounds_min.y == -32.0F &&
			page_packed.bounds_min.z == 96.0F &&
			page_packed.bounds_max.x == 96.0F &&
			page_packed.bounds_max.y == 0.0F &&
			page_packed.bounds_max.z == 128.0F,
		"page-field resident bounds changed"
	);
	WtGpuMeshingShadowRequest invalid_request = packing_request();
	invalid_request.records[0].regular_input.samples[0].density =
		std::numeric_limits<float>::infinity();
	require(
		!wt_pack_gpu_meshing_input(invalid_request, packed, packing_error),
		"non-finite resident sample was accepted"
	);

	WtGpuMeshingShadowQueue pre_mesh_queue;
	require(
		pre_mesh_queue.begin(
			2, false, WtGpuMeshingCaptureStage::PreMeshField
		) && pre_mesh_queue.captures_pre_mesh_field(),
		"pre-mesh resident queue did not retain its capture stage"
	);
	WtGpuMeshingShadowCapture wrong_stage_capture = capture_for(7);
	require(
		!pre_mesh_queue.capture(wrong_stage_capture),
		"pre-mesh resident queue accepted post-mesh authority input"
	);
	wrong_stage_capture.capture_stage = WtGpuMeshingCaptureStage::PreMeshField;
	require(
		pre_mesh_queue.capture(std::move(wrong_stage_capture)) &&
			pre_mesh_queue.metrics().pre_mesh_field_captures == 1,
		"pre-mesh resident queue did not account direct field input"
	);
	pre_mesh_queue.end();

	WtGpuMeshingShadowQueue reservation_queue;
	require(reservation_queue.begin(2, true), "reservation queue did not start");
	std::uint64_t capacity_notifications = 0;
	reservation_queue.set_capacity_available_notifier(
		[&capacity_notifications]() { ++capacity_notifications; }
	);
	WtGpuMeshingShadowCapture reserved_capture = capture_for(8);
	reserved_capture.cpu_visual_mesh_omitted = true;
	const std::uint64_t reservation_id =
		reservation_queue.reserve_capture_slots(reserved_capture.job);
	require(reservation_id != 0, "bounded capture reservation failed");
	require(
		reservation_queue.metrics().reserved_capture_slots == 2,
		"publication reservation did not retain atomic surface capacity"
	);
	require(
		reservation_queue.capture_reserved(
			reservation_id, std::move(reserved_capture)
		),
		"reserved terrain capture failed"
	);
	WtGpuMeshingShadowCapture water_capture = capture_for(8);
	water_capture.surface = WtGpuMeshingShadowSurface::StaticWater;
	require(
		reservation_queue.capture_reserved(
			reservation_id, std::move(water_capture)
		),
		"reserved water capture failed"
	);
	require(
		reservation_queue.metrics().reserved_capture_slots == 0 &&
			reservation_queue.metrics().reserved_captures == 2 &&
			reservation_queue.metrics().cpu_visual_mesh_omitted_captures == 1,
		"reserved capture accounting did not converge"
	);
	require(
		reservation_queue.reserve_capture_slots(capture_for(7).job) == 0,
		"queued reserved captures did not retain hard capacity"
	);
	WtGpuMeshingShadowRequest reserved_terrain;
	require(
		reservation_queue.pop(reserved_terrain) &&
			reserved_terrain.cpu_visual_mesh_omitted,
		"reserved terrain request did not pop"
	);
	require(
		reservation_queue.reject_resident(
			reserved_terrain.request_id,
			identity_for(reserved_terrain),
			"test release"
		).status == WtGpuMeshingResidentValidationStatus::Rejected,
		"reserved terrain request did not release"
	);
	require(
		capacity_notifications == 1,
		"resident rejection did not notify blocked capture admission"
	);
	WtGpuMeshingShadowRequest reserved_water;
	require(
		reservation_queue.pop(reserved_water),
		"reserved water request did not pop"
	);
	require(
		reservation_queue.reject_resident(
			reserved_water.request_id,
			identity_for(reserved_water),
			"test release"
		).status == WtGpuMeshingResidentValidationStatus::Rejected,
		"reserved water request did not release"
	);
	require(
		capacity_notifications == 2,
		"second resident rejection did not notify blocked capture admission"
	);
	const std::uint64_t releasable_id =
		reservation_queue.reserve_capture_slots(capture_for(9).job);
	require(releasable_id != 0, "releasable reservation failed");
	reservation_queue.release_capture_slots(releasable_id);
	require(
		reservation_queue.metrics().reserved_capture_slots == 0 &&
			reservation_queue.metrics().released_capture_slots == 2 &&
			capacity_notifications == 3,
		"unused reservation did not release"
	);
	reservation_queue.set_capacity_available_notifier({});

	WtGpuMeshingShadowQueue priority_queue;
	require(priority_queue.begin(2), "priority reservation queue did not start");
	WtGpuMeshingShadowCapture low_priority = capture_for(10);
	low_priority.job.priority = 10;
	require(priority_queue.capture(low_priority), "first low-priority capture failed");
	low_priority.surface = WtGpuMeshingShadowSurface::StaticWater;
	require(priority_queue.capture(low_priority), "second low-priority capture failed");
	WtGpuMeshingShadowCapture high_priority = capture_for(11);
	high_priority.job.priority = 20;
	const std::uint64_t priority_reservation =
		priority_queue.reserve_capture_slots(high_priority.job);
	require(priority_reservation != 0, "higher-priority reservation was rejected");
	require(
		priority_queue.metrics().superseded_queued_requests == 2 &&
			priority_queue.metrics().queued_requests == 0,
		"higher-priority reservation did not evict the complete queued job"
	);
	WtGpuMeshingShadowCapture wrong_reserved_capture = high_priority;
	wrong_reserved_capture.job.generation.value += 1;
	require(
		!priority_queue.capture_reserved(
			priority_reservation, std::move(wrong_reserved_capture)
		),
		"reservation accepted a different job identity"
	);
	require(
		priority_queue.capture_reserved(
			priority_reservation, std::move(high_priority)
		),
		"higher-priority reserved capture failed"
	);
	priority_queue.release_capture_slots(priority_reservation);

	WtGpuMeshingShadowQueue gpu_only_priority_queue;
	require(
		gpu_only_priority_queue.begin(
			2, true, WtGpuMeshingCaptureStage::PreMeshField
		),
		"GPU-only priority reservation queue did not start"
	);
	WtGpuMeshingShadowCapture retained_low_priority = capture_for(30);
	retained_low_priority.capture_stage = WtGpuMeshingCaptureStage::PreMeshField;
	retained_low_priority.cpu_visual_mesh_omitted = true;
	retained_low_priority.job.priority = 10;
	const std::uint64_t retained_reservation =
		gpu_only_priority_queue.reserve_capture_slots(
			retained_low_priority.job
		);
	require(retained_reservation != 0,
		"GPU-only retained reservation failed");
	require(gpu_only_priority_queue.capture_reserved(
			retained_reservation, retained_low_priority
		), "GPU-only retained terrain capture failed");
	retained_low_priority.surface = WtGpuMeshingShadowSurface::StaticWater;
	require(gpu_only_priority_queue.capture_reserved(
			retained_reservation, retained_low_priority
		), "GPU-only retained water capture failed");
	WtGpuMeshingShadowCapture unrelated_high_priority = capture_for(31);
	unrelated_high_priority.capture_stage = WtGpuMeshingCaptureStage::PreMeshField;
	unrelated_high_priority.job.key.x += 1;
	unrelated_high_priority.job.world_revision = 1;
	unrelated_high_priority.job.priority = 20;
	require(
		gpu_only_priority_queue.reserve_capture_slots(
			unrelated_high_priority.job
		) == 0,
		"GPU-only admission evicted an unrelated accepted chunk"
	);
	require(
		gpu_only_priority_queue.metrics().queued_requests == 2 &&
			gpu_only_priority_queue.metrics().superseded_queued_requests == 0 &&
			gpu_only_priority_queue.metrics().capture_reservation_rejections == 1,
		"GPU-only admission did not preserve lossless backpressure"
	);

	WtGpuMeshingShadowQueue dequeue_queue;
	require(dequeue_queue.begin(4), "priority dequeue queue did not start");
	WtGpuMeshingShadowCapture low_dequeue = capture_for(12);
	low_dequeue.job.key.x = 1;
	low_dequeue.job.priority = 10;
	WtGpuMeshingShadowCapture high_dequeue = capture_for(12);
	high_dequeue.job.key.x = 2;
	high_dequeue.job.priority = 20;
	require(dequeue_queue.capture(low_dequeue), "low-priority dequeue capture failed");
	require(dequeue_queue.capture(high_dequeue), "high-priority dequeue capture failed");
	WtGpuMeshingShadowRequest priority_request;
	require(
		dequeue_queue.pop(priority_request) && priority_request.job.key.x == 2,
		"dequeue did not select authoritative scheduler priority"
	);
	require(
		dequeue_queue.metrics().priority_dequeues == 1,
		"priority dequeue was not measured"
	);
	require(
		dequeue_queue.reject_resident(
			priority_request.request_id,
			identity_for(priority_request),
			"test release"
		).status == WtGpuMeshingResidentValidationStatus::Rejected,
		"priority dequeue request did not release"
	);
	WtGpuMeshingShadowCapture stale_dequeue = capture_for(13);
	stale_dequeue.job.key.x = 3;
	stale_dequeue.job.world_revision = 1;
	WtGpuMeshingShadowCapture current_dequeue = capture_for(14);
	current_dequeue.job.key.x = 3;
	current_dequeue.job.world_revision = 2;
	require(dequeue_queue.capture(stale_dequeue), "stale dequeue capture failed");
	require(dequeue_queue.capture(current_dequeue), "current dequeue capture failed");
	WtGpuMeshingShadowRequest current_request;
	require(
		dequeue_queue.pop(current_request) &&
			current_request.job.world_revision == 2,
		"dequeue did not select the newest world revision"
	);
	require(
		dequeue_queue.metrics().dequeue_superseded_requests == 1 &&
			dequeue_queue.metrics().queued_requests == 1,
		"dequeue discarded an unaffected older-revision chunk"
	);
	WtGpuMeshingShadowRequest unaffected_request;
	require(
		dequeue_queue.pop(unaffected_request) &&
			unaffected_request.job.key.x == 1,
		"dequeue did not retain the unaffected older-revision chunk"
	);
	require(
		dequeue_queue.reject_resident(
			unaffected_request.request_id,
			identity_for(unaffected_request),
			"test release"
		).status == WtGpuMeshingResidentValidationStatus::Rejected,
		"unaffected dequeue request did not release"
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
	WtGpuMeshingShadowRequest newer_request;
	require(
		queue.pop(newer_request) && newer_request.job.generation.value == 41,
		"newer generation was not selected at dequeue"
	);
	WtGpuMeshingShadowRequest obsolete_request;
	require(
		!queue.pop(obsolete_request),
		"obsolete generation entered flight after dequeue coalescing"
	);
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
	require(
		metrics.dequeue_superseded_requests == 1,
		"dequeue supersession metric changed after restart"
	);
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
			7
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
		"queued_superseded=1 pre_mesh_field=1 cpu_topology_calls=0\n";
	return 0;
}
