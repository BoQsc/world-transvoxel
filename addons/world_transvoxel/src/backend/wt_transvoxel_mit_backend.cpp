#include "backend/wt_transvoxel_mit_backend.h"

// The upstream file is deliberately included in this MIT adapter translation
// unit so official table types and symbols do not leak into project headers.
#include "../../thirdparty/transvoxel_mit/Transvoxel.cpp"

namespace world_transvoxel {
namespace {

static_assert(sizeof(regularCellClass) == 256);
static_assert(sizeof(transitionCellClass) == 512);
static_assert(sizeof(regularCellData) / sizeof(regularCellData[0]) == 16);
static_assert(sizeof(transitionCellData) / sizeof(transitionCellData[0]) == 56);

constexpr WtMeshingBackendInfo kBackendInfo = {
	"transvoxel_mit_official",
	"MIT",
	"51a494f03c5b024cd153b596bcc7152eb3cc93a6",
	256,
	512,
	true,
};

const WtTransvoxelMitBackend kBackend;

} // namespace

const WtMeshingBackendInfo &WtTransvoxelMitBackend::get_info() const noexcept {
	return kBackendInfo;
}

bool WtTransvoxelMitBackend::is_available() const noexcept {
	return true;
}

const WtMeshingBackend &wt_get_transvoxel_mit_backend() noexcept {
	return kBackend;
}

} // namespace world_transvoxel
