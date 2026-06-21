#include "services/wt_runtime_config.h"
#include "storage/wt_hash256.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace wt = world_transvoxel;

namespace {

int failure_count = 0;

void check(bool condition, const char *message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failure_count;
	}
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
	for (std::size_t index = 0; index < 8; ++index) {
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
	}
}

void append_status(
	std::vector<std::uint8_t> &bytes,
	wt::WtRuntimeConfigStatus status
) {
	bytes.push_back(static_cast<std::uint8_t>(status));
}

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

void test_defaults(std::vector<std::uint8_t> &evidence) {
	const wt::WtRuntimeConfig config;
	check(wt::wt_validate_runtime_config(config) ==
		wt::WtRuntimeConfigStatus::Ok, "default runtime config is invalid");
	check(config.schema == 1 && config.active_chunk_capacity == 256 &&
		config.viewer_capacity == 8 &&
		config.demand_capacity_per_viewer == 4096 &&
		config.storage_request_capacity == 256 &&
		config.storage_completion_capacity == 256 &&
		config.encoded_page_entry_capacity == 256 &&
		config.decoded_page_entry_capacity == 128 &&
		config.mesh_entry_capacity == 128 &&
		config.render_entry_capacity == 128 &&
		config.collision_entry_capacity == 64 &&
		config.trace_event_capacity == 65536 &&
		config.render_apply_budget == 4 &&
		config.collision_apply_budget == 2 &&
		config.collision_activation_distance == 96.0 &&
		config.collision_deactivation_distance == 128.0,
		"default runtime config values changed");
	append_u64(evidence, config.active_chunk_capacity);
	append_u64(evidence, config.viewer_capacity);
	append_u64(evidence, config.demand_capacity_per_viewer);
	append_u64(evidence, config.encoded_page_byte_capacity);
	append_u64(evidence, config.decoded_page_byte_capacity);
	append_u64(evidence, config.mesh_byte_capacity);
	append_u64(evidence, config.render_byte_capacity);
	append_u64(evidence, config.collision_byte_capacity);
	append_u64(evidence, config.trace_event_capacity);
	append_u64(evidence, config.render_apply_budget);
	append_u64(evidence, config.collision_apply_budget);
}

void expect_status(
	const wt::WtRuntimeConfig &config,
	wt::WtRuntimeConfigStatus expected,
	const char *message,
	std::vector<std::uint8_t> &evidence
) {
	const wt::WtRuntimeConfigStatus actual =
		wt::wt_validate_runtime_config(config);
	check(actual == expected, message);
	check(wt::wt_runtime_config_status_message(actual)[0] != '\0',
		"runtime config status has no message");
	append_status(evidence, actual);
}

void test_rejections(std::vector<std::uint8_t> &evidence) {
	wt::WtRuntimeConfig config;
	config.schema = 2;
	expect_status(config, wt::WtRuntimeConfigStatus::UnsupportedSchema,
		"future schema was accepted", evidence);
	config = {};
	config.active_chunk_capacity = 0;
	expect_status(config, wt::WtRuntimeConfigStatus::InvalidActiveChunkCapacity,
		"zero active chunk capacity was accepted", evidence);
	config = {};
	config.viewer_capacity = 0;
	expect_status(config, wt::WtRuntimeConfigStatus::InvalidViewerCapacity,
		"zero viewer capacity was accepted", evidence);
	config = {};
	config.demand_capacity_per_viewer = 0;
	expect_status(config, wt::WtRuntimeConfigStatus::InvalidDemandCapacity,
		"zero viewer demand capacity was accepted", evidence);
	config = {};
	config.viewer_capacity = 1024;
	config.demand_capacity_per_viewer = 65536;
	expect_status(config, wt::WtRuntimeConfigStatus::InvalidTotalDemandCapacity,
		"excess total demand capacity was accepted", evidence);
	config = {};
	config.storage_request_capacity = 0;
	expect_status(config, wt::WtRuntimeConfigStatus::InvalidStorageQueueCapacity,
		"zero storage queue capacity was accepted", evidence);
	config = {};
	config.encoded_page_byte_capacity = 0;
	expect_status(config, wt::WtRuntimeConfigStatus::InvalidStorageCacheCapacity,
		"zero storage cache bytes were accepted", evidence);
	config = {};
	config.mesh_entry_capacity = 0;
	expect_status(config, wt::WtRuntimeConfigStatus::InvalidResourceCacheCapacity,
		"zero resource cache entries were accepted", evidence);
	config = {};
	config.trace_event_capacity = 0;
	expect_status(config, wt::WtRuntimeConfigStatus::InvalidTraceCapacity,
		"zero trace capacity was accepted", evidence);
	config = {};
	config.render_apply_budget = 129;
	expect_status(config, wt::WtRuntimeConfigStatus::InvalidApplyBudget,
		"excess render budget was accepted", evidence);
	config = {};
	config.collision_activation_distance = 129.0;
	expect_status(config, wt::WtRuntimeConfigStatus::InvalidCollisionDistance,
		"inverted collision distances were accepted", evidence);
}

} // namespace

int main() {
	std::vector<std::uint8_t> evidence;
	test_defaults(evidence);
	test_rejections(evidence);
	if (failure_count != 0) {
		std::fprintf(stderr, "PRODUCTION_CONFIG_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("PRODUCTION_CONFIG_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf("PRODUCTION_CONFIG_PASS schema=1 rejection_cases=11\n");
	return 0;
}
