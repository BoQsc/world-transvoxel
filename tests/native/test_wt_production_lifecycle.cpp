#include "services/wt_world_lifecycle.h"
#include "storage/wt_hash256.h"
#include "wt_production_world_fixture.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace wt = world_transvoxel;
namespace wtt = world_transvoxel::testing;

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

void print_hash(const wt::WtHash256 &hash) {
	for (std::uint8_t byte : hash) {
		std::printf("%02x", static_cast<unsigned int>(byte));
	}
	std::printf("\n");
}

class FixtureRoot {
public:
	FixtureRoot() {
		path = std::filesystem::temp_directory_path() /
			("wt_production_lifecycle_" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()
			));
	}
	~FixtureRoot() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
	std::filesystem::path path;
};

void stop_and_wait(wt::WtWorldLifecycleService &service) {
	const wt::WtWorldLifecycleStatus status = service.request_stop();
	check(status == wt::WtWorldLifecycleStatus::Ok ||
		status == wt::WtWorldLifecycleStatus::AlreadyStopping,
		"lifecycle stop request failed");
	check(service.wait_for_state(
		wt::WtWorldLifecycleState::Stopped,
		std::chrono::seconds(3)
	), "lifecycle did not stop");
}

void test_success(
	const std::filesystem::path &world_path,
	const std::filesystem::path &object_root,
	std::vector<std::uint8_t> &evidence
) {
	wt::WtWorldLifecycleService service({});
	check(service.state() == wt::WtWorldLifecycleState::Stopped,
		"lifecycle did not begin stopped");
	check(service.start(world_path, object_root) ==
		wt::WtWorldLifecycleStatus::Ok,
		"valid lifecycle startup was rejected");
	check(service.wait_for_state(
		wt::WtWorldLifecycleState::Running,
		std::chrono::seconds(3)
	), "valid lifecycle did not become running");
	check(service.source_revision() == 7001 &&
		service.world_revision() == 12 && service.page_count() == 0,
		"running lifecycle metadata mismatch");
	check(service.last_storage_status() == wt::WtAsyncStorageStatus::Ok,
		"running lifecycle retained a storage failure");
	check(service.start(world_path, object_root) ==
		wt::WtWorldLifecycleStatus::InvalidState,
		"double lifecycle startup was accepted");
	append_u64(evidence, static_cast<std::uint8_t>(service.state()));
	append_u64(evidence, service.source_revision());
	append_u64(evidence, service.world_revision());
	append_u64(evidence, service.page_count());
	stop_and_wait(service);
	check(service.source_revision() == 0 && service.world_revision() == 0 &&
		service.page_count() == 0,
		"stopped lifecycle retained manifest metadata");

	check(service.start(world_path, object_root) ==
		wt::WtWorldLifecycleStatus::Ok,
		"stopped lifecycle could not restart");
	check(service.wait_for_state(
		wt::WtWorldLifecycleState::Running,
		std::chrono::seconds(3)
	), "restarted lifecycle did not run");
	service.shutdown_blocking();
	check(service.state() == wt::WtWorldLifecycleState::Stopped,
		"blocking lifecycle shutdown did not finish");
}

void test_failures(
	const std::filesystem::path &root,
	std::vector<std::uint8_t> &evidence
) {
	wt::WtWorldLifecycleService service({});
	check(service.start({}, root) == wt::WtWorldLifecycleStatus::InvalidPath,
		"empty lifecycle manifest path was accepted");
	check(service.start(root / "missing.wtworld", root) ==
		wt::WtWorldLifecycleStatus::Ok,
		"missing manifest startup was not asynchronous");
	check(service.wait_for_state(
		wt::WtWorldLifecycleState::Failed,
		std::chrono::seconds(3)
	), "missing manifest did not reach failed state");
	check(service.last_storage_status() == wt::WtAsyncStorageStatus::InvalidPath,
		"missing manifest failure status mismatch");
	append_u64(evidence,
		static_cast<std::uint8_t>(service.last_storage_status()));
	stop_and_wait(service);

	const std::filesystem::path corrupt = root / "corrupt.wtworld";
	std::ofstream output(corrupt, std::ios::binary | std::ios::trunc);
	output.write("bad!", 4);
	output.close();
	check(service.start(corrupt, root) == wt::WtWorldLifecycleStatus::Ok,
		"corrupt manifest startup was not asynchronous");
	check(service.wait_for_state(
		wt::WtWorldLifecycleState::Failed,
		std::chrono::seconds(3)
	), "corrupt manifest did not reach failed state");
	check(service.last_storage_status() ==
		wt::WtAsyncStorageStatus::ManifestFailure,
		"corrupt manifest failure status mismatch");
	append_u64(evidence,
		static_cast<std::uint8_t>(service.last_storage_status()));
	stop_and_wait(service);

	wt::WtRuntimeConfig invalid;
	invalid.storage_request_capacity = 0;
	wt::WtWorldLifecycleService invalid_service(invalid);
	check(invalid_service.start(corrupt, root) ==
		wt::WtWorldLifecycleStatus::InvalidConfiguration,
		"invalid lifecycle configuration was accepted");
}

void test_start_stop_race(
	const std::filesystem::path &world_path,
	const std::filesystem::path &object_root
) {
	wt::WtWorldLifecycleService service({});
	check(service.start(world_path, object_root) ==
		wt::WtWorldLifecycleStatus::Ok,
		"start-stop fixture startup failed");
	stop_and_wait(service);
	check(service.state() == wt::WtWorldLifecycleState::Stopped,
		"start-stop race retained lifecycle state");
}

int write_godot_fixture(const std::filesystem::path &root) {
	std::filesystem::path world_path;
	if (!wtt::wt_write_production_world_fixture(root, 7001, 12, world_path)) {
		std::fprintf(stderr, "PRODUCTION_LIFECYCLE_FIXTURE_FAIL\n");
		return 1;
	}
	std::filesystem::path streaming_path;
	if (!wtt::wt_write_production_streaming_fixture(
			root, 7001, 12, streaming_path
		)) {
		std::fprintf(stderr, "PRODUCTION_STREAMING_FIXTURE_FAIL\n");
		return 1;
	}
	std::filesystem::path transition_path;
	if (!wtt::wt_write_production_transition_fixture(
			root, 8001, 13, transition_path
		)) {
		std::fprintf(stderr, "PRODUCTION_TRANSITION_FIXTURE_FAIL\n");
		return 1;
	}
	std::printf("PRODUCTION_LIFECYCLE_FIXTURE_PASS %s\n",
		world_path.string().c_str());
	std::printf("PRODUCTION_STREAMING_FIXTURE_PASS %s\n",
		streaming_path.string().c_str());
	std::printf("PRODUCTION_TRANSITION_FIXTURE_PASS %s\n",
		transition_path.string().c_str());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	if (argc == 3 &&
		std::string_view(argv[1]) == "--write-godot-fixture") {
		return write_godot_fixture(argv[2]);
	}
	if (argc != 1) {
		std::fprintf(stderr, "usage: %s [--write-godot-fixture DIR]\n", argv[0]);
		return 2;
	}
	FixtureRoot fixture;
	std::filesystem::path world_path;
	check(wtt::wt_write_production_world_fixture(
		fixture.path, 7001, 12, world_path
	), "production lifecycle fixture write failed");
	std::vector<std::uint8_t> evidence;
	test_success(world_path, fixture.path, evidence);
	test_failures(fixture.path, evidence);
	test_start_stop_race(world_path, fixture.path);
	if (failure_count != 0) {
		std::fprintf(stderr,
			"PRODUCTION_LIFECYCLE_FAIL failures=%d\n", failure_count);
		return 1;
	}
	std::printf("PRODUCTION_LIFECYCLE_HASH ");
	print_hash(wt::wt_sha256(evidence.data(), evidence.size()));
	std::printf(
		"PRODUCTION_LIFECYCLE_PASS states=5 async_failures=2 restarts=1\n"
	);
	return 0;
}
