#include "bake/wt_chunk_baker.h"
#include "editing/wt_edit_spatial_index.h"
#include "physics/wt_collision_builder.h"
#include "render/wt_render_payload.h"
#include "services/wt_chunk_application.h"
#include "services/wt_chunk_resource_cache.h"
#include "services/wt_edit_runtime_replacement.h"
#include "services/wt_page_meshing_runtime_owner.h"
#include "storage/wt_hash256.h"
#include "storage/wt_storage_page_cache.h"
#include "streaming/wt_stream_scheduler.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace wt = world_transvoxel;

namespace {

class RecordingPageMeshingOwner final :
		public wt::WtPageMeshingRuntimeOwner {
public:
	wt::WtPageMeshingRuntimeOwnerStatus cancel_owned_generation(
		const wt::WtChunkKey &,
		wt::WtGenerationToken
	) override {
		++cancelled;
		return wt::WtPageMeshingRuntimeOwnerStatus::Ok;
	}

	wt::WtPageMeshingRuntimeOwnerStatus release_owned_chunk(
		const wt::WtChunkKey &
	) override {
		return wt::WtPageMeshingRuntimeOwnerStatus::NotFound;
	}

	wt::WtPageMeshingRuntimeOwnerStatus reprioritize_owned_chunk(
		const wt::WtChunkKey &,
		wt::WtGenerationToken,
		std::int32_t
	) override {
		return wt::WtPageMeshingRuntimeOwnerStatus::NotFound;
	}

	std::size_t cancelled = 0;
};

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

wt::WtId128 id(std::uint64_t seed) {
	wt::WtId128 value{};
	for (std::size_t index = 0; index < value.size(); ++index) {
		value[index] = static_cast<std::uint8_t>(seed + index * 17U);
	}
	return value;
}

wt::WtEditTransaction transaction(
	std::uint64_t source_revision,
	std::uint64_t base_revision,
	std::int64_t center,
	std::uint64_t identity
) {
	wt::WtEditTransaction result;
	result.source_revision = source_revision;
	result.transaction_id = id(identity);
	result.base_revision = base_revision;
	result.committed_revision = base_revision + 1;
	result.author_id = 19;
	wt::WtEditCommand command;
	command.command_id = id(identity + 71);
	command.world_revision = result.committed_revision;
	command.operation = wt::WtEditOperation::AddDensity;
	command.shape = wt::WtEditShape::Sphere;
	command.density_value = -0.5F;
	command.sphere = {
		center * wt::kWtEditCoordinateScale,
		center * wt::kWtEditCoordinateScale,
		center * wt::kWtEditCoordinateScale,
		static_cast<std::uint64_t>(wt::kWtEditCoordinateScale / 4),
	};
	check(wt::wt_edit_sphere_bounds(command.sphere, command.bounds),
		"edit replacement bounds construction failed");
	result.commands.push_back(command);
	return result;
}

class CacheSource final : public wt::WtChunkSampleSource {
public:
	bool sample(
		const wt::WtGridPoint &point,
		wt::WtScalarSample &output
	) const noexcept override {
		output.density = static_cast<float>(point.x - point.y + point.z) * 0.03125F;
		output.material = static_cast<std::uint16_t>(
			static_cast<std::uint64_t>(point.x * 3 + point.y * 5 + point.z * 7)
		);
		return true;
	}
};

std::vector<wt::WtBakedChunkPage> bake_pages(
	const std::vector<wt::WtChunkKey> &keys,
	std::uint64_t source_revision
) {
	CacheSource source;
	wt::WtChunkBaker baker(keys.size());
	std::vector<wt::WtBakedChunkPage> pages;
	check(baker.bake(keys, source_revision, source, pages) ==
		wt::WtChunkBakeStatus::Ok, "edit replacement page bake failed");
	return pages;
}

const wt::WtBakedChunkPage *find_page(
	const std::vector<wt::WtBakedChunkPage> &pages,
	const wt::WtChunkKey &key
) {
	for (const wt::WtBakedChunkPage &page : pages) {
		if (page.key == key) return &page;
	}
	return nullptr;
}

wt::WtPageLoadCompletion page_completion(
	const wt::WtBakedChunkPage &page,
	wt::WtGenerationToken generation
) {
	wt::WtPageLoadCompletion result;
	result.key = page.key;
	result.generation = generation;
	result.status = wt::WtPageLoadStatus::Ok;
	result.page_bytes =
		std::make_shared<const std::vector<std::uint8_t>>(page.bytes);
	return result;
}

wt::WtCellVertex vertex(float x, float z, std::uint16_t material) {
	wt::WtCellVertex result;
	result.position = { x, 0.0F, z };
	result.normal = { 0.0F, 1.0F, 0.0F };
	result.material = material;
	return result;
}

std::shared_ptr<const wt::WtChunkMeshResult> make_mesh(
	const wt::WtChunkKey &key,
	std::uint16_t material
) {
	auto mesh = std::make_shared<wt::WtChunkMeshResult>();
	mesh->key = key;
	mesh->world_origin = wt::wt_chunk_bounds(key).minimum;
	mesh->regular.vertices = {
		vertex(0.0F, 0.0F, material),
		vertex(1.0F, 0.0F, material),
		vertex(0.0F, 1.0F, material),
	};
	mesh->regular.indices = { 0, 1, 2 };
	return mesh;
}

std::shared_ptr<const wt::WtRenderPayload> make_render(
	const wt::WtChunkMeshResult &mesh,
	wt::WtGenerationToken generation
) {
	auto render = std::make_shared<wt::WtRenderPayload>();
	check(wt::wt_build_render_payload(mesh, generation, *render) ==
		wt::WtRenderBuildStatus::Ok, "edit replacement render build failed");
	return render;
}

std::shared_ptr<const wt::WtCollisionPayload> make_collision(
	const wt::WtRenderPayload &render
) {
	auto collision = std::make_shared<wt::WtCollisionPayload>();
	check(wt::wt_build_collision_payload(render, {}, *collision) ==
		wt::WtCollisionBuildStatus::Ok,
		"edit replacement collision build failed");
	return collision;
}

struct RenderSink final : wt::WtRenderSink {
	std::size_t calls = 0;
	bool apply_render(const wt::WtRenderPayload &) override {
		++calls;
		return true;
	}
};

struct CollisionSink final : wt::WtCollisionSink {
	std::size_t calls = 0;
	bool apply_collision(const wt::WtCollisionPayload &) override {
		++calls;
		return true;
	}
};

void complete(wt::WtStreamScheduler &scheduler, const wt::WtChunkJob &job) {
	check(scheduler.submit_completion({ job.key, job.generation, job.stage, true }) ==
		wt::WtSchedulerStatus::Ok, "edit replacement completion submit failed");
	check(scheduler.apply_completions(1) == 1,
		"edit replacement completion apply failed");
}

wt::WtGenerationToken request_ready(
	wt::WtStreamScheduler &scheduler,
	const wt::WtChunkKey &key,
	std::uint64_t source_revision,
	std::uint64_t world_revision,
	std::int32_t priority
) {
	check(scheduler.request_chunk_version(
		key, source_revision, world_revision, priority
	) == wt::WtSchedulerStatus::Ok, "initial versioned request failed");
	wt::WtChunkJob job;
	check(scheduler.pop_job(job) && job.stage == wt::WtChunkJobStage::Sample &&
		job.source_revision == source_revision &&
		job.world_revision == world_revision,
		"initial versioned sample job mismatch");
	complete(scheduler, job);
	check(scheduler.pop_job(job) && job.stage == wt::WtChunkJobStage::Mesh &&
		job.world_revision == world_revision,
		"initial versioned mesh job mismatch");
	complete(scheduler, job);
	const wt::WtChunkRecord *record = scheduler.find_record(key);
	check(record != nullptr && record->lifecycle == wt::WtChunkLifecycle::Ready,
		"initial versioned request did not become ready");
	return record == nullptr ? wt::WtGenerationToken{} : record->generation;
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
	for (std::size_t index = 0; index < 8; ++index) {
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
	}
}

void append_key(std::vector<std::uint8_t> &bytes, const wt::WtChunkKey &key) {
	append_u64(bytes, static_cast<std::uint32_t>(key.x));
	append_u64(bytes, static_cast<std::uint32_t>(key.y));
	append_u64(bytes, static_cast<std::uint32_t>(key.z));
	bytes.push_back(key.lod);
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void test_end_to_end(std::vector<std::uint8_t> &evidence) {
	constexpr std::uint64_t source_revision = 500;
	const std::vector<wt::WtChunkKey> keys = {
		{ 0, 0, 0, 0 },
		{ 0, 0, 0, 1 },
		{ 4, 0, 0, 0 },
	};
	wt::WtEditSpatialIndex spatial(keys.size(), 256, keys.size());
	check(spatial.rebuild(keys) == wt::WtEditSpatialStatus::Ok,
		"edit replacement spatial rebuild failed");
	wt::WtStreamScheduler scheduler(keys.size(), 16, 16, 1);
	wt::WtChunkApplicationService application(keys.size(), 16, 16);
	wt::WtStoragePageCache page_cache({ 8, wt::kWtMaximumContainerSize,
		8, wt::kWtMaximumContainerSize });
	wt::WtChunkResourceCache resource_cache({
		8, wt::kWtMaximumResourceCacheBytes,
		8, wt::kWtMaximumResourceCacheBytes,
		8, wt::kWtMaximumResourceCacheBytes,
	});
	RenderSink render_sink;
	CollisionSink collision_sink;
	std::vector<wt::WtGenerationToken> generations;
	std::vector<std::shared_ptr<const wt::WtChunkMeshResult>> meshes;
	std::vector<std::shared_ptr<const wt::WtRenderPayload>> renders;
	std::vector<std::shared_ptr<const wt::WtCollisionPayload>> collisions;
	const std::vector<wt::WtBakedChunkPage> pages =
		bake_pages(keys, source_revision);
	check(pages.size() == keys.size(), "edit replacement page count mismatch");

	for (std::size_t index = 0; index < keys.size(); ++index) {
		const wt::WtBakedChunkPage *page = find_page(pages, keys[index]);
		check(page != nullptr, "edit replacement baked page lookup failed");
		const wt::WtGenerationToken generation = request_ready(
			scheduler, keys[index], source_revision, 7,
			static_cast<std::int32_t>(30 - index * 10)
		);
		generations.push_back(generation);
		meshes.push_back(make_mesh(keys[index],
			static_cast<std::uint16_t>(index + 1)));
		renders.push_back(make_render(*meshes.back(), generation));
		collisions.push_back(make_collision(*renders.back()));
		const bool collision_required = index != 1;
		check(application.expect_chunk(
			keys[index], generation, collision_required
		) == wt::WtApplicationStatus::Ok,
			"initial edit application expectation failed");
		check(application.submit_render(renders.back()) ==
			wt::WtApplicationStatus::Ok,
			"initial edit render submission failed");
		if (collision_required) {
			check(application.submit_collision(collisions.back()) ==
				wt::WtApplicationStatus::Ok,
				"initial edit collision submission failed");
		}
		check(resource_cache.insert_mesh(
			meshes.back(), generation, generation
		) == wt::WtChunkResourceCacheStatus::Ok &&
			resource_cache.insert_render(renders.back(), generation) ==
				wt::WtChunkResourceCacheStatus::Ok &&
			resource_cache.insert_collision(collisions.back(), generation) ==
				wt::WtChunkResourceCacheStatus::Ok,
			"initial edit resource cache insertion failed");
		check(page_cache.accept_completion(
			page_completion(*page, generation), generation
		) == wt::WtStoragePageCacheStatus::Ok,
			"initial edit page cache insertion failed");
		std::shared_ptr<const wt::WtChunkPage> decoded;
		check(page_cache.find_or_decode(keys[index], source_revision, decoded) ==
			wt::WtStoragePageCacheStatus::Ok && decoded,
			"initial edit page decode failed");
	}
	application.apply(16, 16, render_sink, collision_sink);
	for (const wt::WtChunkKey &key : keys) {
		const wt::WtChunkApplicationRecord *record = application.find_record(key);
		check(record != nullptr && record->fully_ready(),
			"initial edit application was not ready");
	}

	check(application.submit_render(renders[0]) == wt::WtApplicationStatus::Ok &&
		application.submit_render(renders[1]) == wt::WtApplicationStatus::Ok &&
		application.submit_collision(collisions[0]) ==
			wt::WtApplicationStatus::Ok,
		"stale application fixture submission failed");
	wt::WtEditRuntimeReplacementService service(keys.size());
	RecordingPageMeshingOwner page_meshing_owner;
	check(service.replace_loaded_chunks(
		transaction(source_revision, 7, 8, 1), spatial, scheduler,
		page_cache, resource_cache, application, &page_meshing_owner
	) == wt::WtEditRuntimeReplacementStatus::Ok,
		"loaded edit replacement failed");
	const auto &replacements = service.get_last_replacements();
	check(replacements.size() == 2 && replacements[0].key == keys[0] &&
		replacements[1].key == keys[1],
		"edit replacement affected the wrong chunks");
	for (std::size_t index = 0; index < replacements.size(); ++index) {
		const auto &replacement = replacements[index];
		const wt::WtChunkRecord *record = scheduler.find_record(replacement.key);
		const wt::WtChunkApplicationRecord *application_record =
			application.find_record(replacement.key);
		check(record != nullptr && record->generation ==
			replacement.replacement_generation &&
			record->generation != replacement.previous_generation &&
			record->source_revision == source_revision &&
			record->world_revision == 8 &&
			record->lifecycle == wt::WtChunkLifecycle::Sampling,
			"edit replacement scheduler state mismatch");
		check(application_record != nullptr &&
			application_record->generation == record->generation &&
			!application_record->visual_ready &&
			!application_record->collision_ready &&
			application_record->collision_required == (index == 0),
			"edit replacement readiness was not reset or preserved");
		check(replacement.evicted_page_entries == 2 &&
			replacement.evicted_resource_entries == 3,
			"edit replacement did not evict all key tiers");
		append_key(evidence, replacement.key);
		append_u64(evidence, replacement.previous_generation.value);
		append_u64(evidence, replacement.replacement_generation.value);
		append_u64(evidence, replacement.replacement_world_revision);
	}
	check(page_cache.encoded_entry_count() == 1 &&
		page_cache.decoded_entry_count() == 1 &&
		resource_cache.mesh_entry_count() == 1 &&
		resource_cache.render_entry_count() == 1 &&
		resource_cache.collision_entry_count() == 1,
		"edit replacement cache residency mismatch");

	check(scheduler.submit_completion({ keys[0], generations[0],
		wt::WtChunkJobStage::Mesh, true }) == wt::WtSchedulerStatus::Ok,
		"stale scheduler fixture submission failed");
	scheduler.apply_completions(1);
	check(scheduler.get_metrics().stale_results == 1,
		"superseded scheduler completion was not stale");
	check(page_cache.accept_completion(
		page_completion(pages[0], generations[0]),
		replacements[0].replacement_generation
	) == wt::WtStoragePageCacheStatus::StaleGeneration,
		"superseded page completion was not stale");
	const std::size_t old_render_calls = render_sink.calls;
	const std::size_t old_collision_calls = collision_sink.calls;
	application.apply(16, 16, render_sink, collision_sink);
	check(render_sink.calls == old_render_calls &&
		collision_sink.calls == old_collision_calls &&
		application.get_metrics().stale_render == 2 &&
		application.get_metrics().stale_collision == 1,
		"superseded application payload reached a sink");

	std::size_t sample_jobs = 0;
	std::size_t mesh_jobs = 0;
	wt::WtChunkJob job;
	while (scheduler.pop_job(job)) {
		check(job.source_revision == source_revision && job.world_revision == 8,
			"replacement job lost source or world revision");
		if (job.stage == wt::WtChunkJobStage::Sample) ++sample_jobs;
		else ++mesh_jobs;
		complete(scheduler, job);
	}
	check(sample_jobs == 2 && mesh_jobs == 2,
		"edit replacement did not remesh every affected chunk");
	for (std::size_t index = 0; index < replacements.size(); ++index) {
		const wt::WtChunkKey key = replacements[index].key;
		const wt::WtChunkRecord *record = scheduler.find_record(key);
		const wt::WtBakedChunkPage *page = find_page(pages, key);
		check(record != nullptr && record->lifecycle == wt::WtChunkLifecycle::Ready,
			"replacement chunk did not become scheduler-ready");
		check(page != nullptr, "replacement baked page lookup failed");
		check(page_cache.accept_completion(
			page_completion(*page, record->generation),
			record->generation
		) == wt::WtStoragePageCacheStatus::Ok,
			"replacement page was not accepted");
		std::shared_ptr<const wt::WtChunkPage> decoded;
		check(page_cache.find_or_decode(key, source_revision, decoded) ==
			wt::WtStoragePageCacheStatus::Ok && decoded,
			"replacement page did not decode");
		auto render = make_render(*meshes[index], record->generation);
		auto collision = make_collision(*render);
		check(resource_cache.insert_mesh(
			meshes[index], record->generation, record->generation
		) == wt::WtChunkResourceCacheStatus::Ok &&
			resource_cache.insert_render(render, record->generation) ==
				wt::WtChunkResourceCacheStatus::Ok &&
			resource_cache.insert_collision(collision, record->generation) ==
				wt::WtChunkResourceCacheStatus::Ok,
			"replacement resources were not cached");
		check(application.submit_render(render) == wt::WtApplicationStatus::Ok,
			"replacement render submission failed");
		if (index == 0) {
			check(application.submit_collision(collision) ==
				wt::WtApplicationStatus::Ok,
				"replacement collision submission failed");
		}
	}
	application.apply(16, 16, render_sink, collision_sink);
	for (const auto &replacement : replacements) {
		const wt::WtChunkApplicationRecord *record =
			application.find_record(replacement.key);
		check(record != nullptr && record->fully_ready(),
			"replacement application did not become fully ready");
	}
	const wt::WtEditRuntimeReplacementMetrics metrics = service.get_metrics();
	check(metrics.completed_transactions == 1 && metrics.replaced_chunks == 2 &&
		metrics.evicted_page_entries == 4 &&
		metrics.evicted_resource_entries == 6 &&
		metrics.cancelled_page_meshing_generations == 2 &&
		page_meshing_owner.cancelled == 2,
		"edit replacement metrics mismatch");
	append_u64(evidence, metrics.replaced_chunks);
	append_u64(evidence, metrics.evicted_page_entries);
	append_u64(evidence, metrics.evicted_resource_entries);
	append_u64(evidence, sample_jobs);
	append_u64(evidence, mesh_jobs);
}

void test_atomic_rejections() {
	constexpr std::uint64_t source_revision = 500;
	const std::vector<wt::WtChunkKey> keys = {
		{ 0, 0, 0, 0 },
		{ 0, 0, 0, 1 },
	};
	wt::WtEditSpatialIndex spatial(keys.size(), 256, keys.size());
	check(spatial.rebuild(keys) == wt::WtEditSpatialStatus::Ok,
		"rejection spatial rebuild failed");
	wt::WtStreamScheduler scheduler(keys.size(), 1, 2, 0);
	wt::WtChunkApplicationService application(keys.size(), 1, 1);
	std::vector<wt::WtGenerationToken> generations;
	for (const wt::WtChunkKey &key : keys) {
		check(scheduler.request_chunk_version(key, source_revision, 7, 1) ==
			wt::WtSchedulerStatus::Ok, "rejection initial request failed");
		wt::WtChunkJob job;
		check(scheduler.pop_job(job), "rejection initial job missing");
		generations.push_back(job.generation);
		check(application.expect_chunk(key, job.generation, false) ==
			wt::WtApplicationStatus::Ok,
			"rejection initial application record failed");
	}
	wt::WtStoragePageCache page_cache({ 2, wt::kWtMaximumContainerSize,
		2, wt::kWtMaximumContainerSize });
	wt::WtChunkResourceCache resource_cache({
		2, wt::kWtMaximumResourceCacheBytes,
		2, wt::kWtMaximumResourceCacheBytes,
		2, wt::kWtMaximumResourceCacheBytes,
	});
	wt::WtEditRuntimeReplacementService service(keys.size());
	check(service.replace_loaded_chunks(
		transaction(source_revision, 7, 8, 20), spatial, scheduler,
		page_cache, resource_cache, application, nullptr
	) == wt::WtEditRuntimeReplacementStatus::JobQueueCapacityExceeded,
		"insufficient batch queue capacity was accepted");
	check(scheduler.queued_job_count() == 0 &&
		service.get_last_replacements().empty(),
		"queue capacity rejection partially replaced a batch");
	for (std::size_t index = 0; index < keys.size(); ++index) {
		const wt::WtChunkRecord *record = scheduler.find_record(keys[index]);
		check(record != nullptr && record->generation == generations[index] &&
			record->world_revision == 7,
			"queue capacity rejection mutated a generation");
	}

	wt::WtChunkApplicationService missing_application(keys.size(), 1, 1);
	check(missing_application.expect_chunk(keys[0], generations[0], false) ==
		wt::WtApplicationStatus::Ok,
		"missing-state fixture application setup failed");
	check(service.replace_loaded_chunks(
		transaction(source_revision, 7, 8, 30), spatial, scheduler,
		page_cache, resource_cache, missing_application, nullptr
	) == wt::WtEditRuntimeReplacementStatus::RuntimeStateMismatch,
		"missing application state was accepted");
	for (std::size_t index = 0; index < keys.size(); ++index) {
		const wt::WtChunkRecord *record = scheduler.find_record(keys[index]);
		check(record != nullptr && record->generation == generations[index],
			"state rejection partially replaced a batch");
	}
	check(service.get_metrics().capacity_rejections == 1 &&
		service.get_metrics().state_rejections == 1,
		"edit replacement rejection metrics mismatch");
}

void test_repeated_bounded_replacement(std::vector<std::uint8_t> &evidence) {
	constexpr std::uint64_t source_revision = 900;
	const wt::WtChunkKey key = { 0, 0, 0, 0 };
	wt::WtEditSpatialIndex spatial(1, 64, 1);
	check(spatial.rebuild({ key }) == wt::WtEditSpatialStatus::Ok,
		"bounded replacement spatial rebuild failed");
	wt::WtStreamScheduler scheduler(1, 1, 1, 0);
	check(scheduler.request_chunk_version(key, source_revision, 7, 1) ==
		wt::WtSchedulerStatus::Ok, "bounded replacement setup failed");
	wt::WtChunkJob job;
	check(scheduler.pop_job(job), "bounded replacement setup job missing");
	wt::WtChunkApplicationService application(1, 1, 1);
	check(application.expect_chunk(key, job.generation, false) ==
		wt::WtApplicationStatus::Ok,
		"bounded replacement application setup failed");
	wt::WtStoragePageCache page_cache({ 1, wt::kWtMaximumContainerSize,
		1, wt::kWtMaximumContainerSize });
	wt::WtChunkResourceCache resource_cache({
		1, wt::kWtMaximumResourceCacheBytes,
		1, wt::kWtMaximumResourceCacheBytes,
		1, wt::kWtMaximumResourceCacheBytes,
	});
	wt::WtEditRuntimeReplacementService service(1);
	constexpr std::size_t cycles = 1000;
	for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
		const std::uint64_t base = 7 + cycle;
		check(service.replace_loaded_chunks(
			transaction(source_revision, base, 8, 100 + cycle),
			spatial, scheduler, page_cache, resource_cache, application, nullptr
		) == wt::WtEditRuntimeReplacementStatus::Ok,
			"bounded repeated edit replacement failed");
		check(scheduler.pop_job(job) && job.world_revision == base + 1,
			"bounded repeated replacement job missing");
	}
	const wt::WtChunkRecord *record = scheduler.find_record(key);
	const wt::WtChunkApplicationRecord *application_record =
		application.find_record(key);
	check(record != nullptr && application_record != nullptr &&
		scheduler.get_records().size() == 1 &&
		application.get_records().size() == 1 &&
		scheduler.queued_job_count() == 0 &&
		record->world_revision == 7 + cycles &&
		application_record->generation == record->generation,
		"repeated edit replacement state grew or diverged");
	check(service.get_metrics().completed_transactions == cycles &&
		service.get_metrics().replaced_chunks == cycles,
		"repeated edit replacement metrics mismatch");
	append_u64(evidence, cycles);
	append_u64(evidence, record == nullptr ? 0 : record->generation.value);
	append_u64(evidence, record == nullptr ? 0 : record->world_revision);
}

} // namespace

int main() {
	std::vector<std::uint8_t> evidence;
	test_end_to_end(evidence);
	test_atomic_rejections();
	test_repeated_bounded_replacement(evidence);
	if (failure_count != 0) {
		std::fprintf(stderr, "M5_EDIT_REPLACEMENT_FAIL failures=%d\n",
			failure_count);
		return 1;
	}
	std::printf("M5_EDIT_REPLACEMENT_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"M5_EDIT_REPLACEMENT_PASS affected=2 stale_pipeline=4 cycles=1000\n"
	);
	return 0;
}
