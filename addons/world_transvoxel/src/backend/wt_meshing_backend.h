#pragma once

namespace world_transvoxel {

struct WtMeshingBackendInfo {
	const char *id;
	const char *license;
	const char *upstream_revision;
	unsigned int regular_case_count;
	unsigned int transition_case_count;
	bool official_reference;
};

class WtMeshingBackend {
public:
	virtual ~WtMeshingBackend() = default;

	virtual const WtMeshingBackendInfo &get_info() const noexcept = 0;
	virtual bool is_available() const noexcept = 0;
};

} // namespace world_transvoxel
