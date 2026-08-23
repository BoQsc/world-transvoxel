#pragma once

#include "backend/wt_meshing_backend.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace world_transvoxel {

constexpr std::size_t kWtMaximumRecordedChunkCells = 5632;

enum class WtRecordedCellType : std::uint8_t {
	Regular,
	Transition,
};

struct WtRecordedMeshingCell {
	WtRecordedCellType type = WtRecordedCellType::Regular;
	WtRegularCellInput regular_input;
	WtTransitionCellInput transition_input;
	WtCellStatus status = WtCellStatus::TopologyFailure;
	WtCellMesh mesh;
	std::uint16_t case_code = 0;
};

struct WtReplayMeshingCell {
	WtRecordedCellType type = WtRecordedCellType::Regular;
	WtTransitionOrientation orientation = WtTransitionOrientation::PositiveX;
	WtCellStatus status = WtCellStatus::TopologyFailure;
	WtCellMesh mesh;
	std::uint16_t case_code = 0;
};

enum class WtReplayMeshingFailure : std::uint8_t {
	None,
	CellSequenceExhausted,
	CellTypeMismatch,
	CaseCodeMismatch,
	OrientationMismatch,
	AuthorityStatusMismatch,
	UnsupportedStatus,
};

class WtRecordingMeshingBackend final : public WtMeshingBackend {
public:
	explicit WtRecordingMeshingBackend(const WtMeshingBackend &authority);

	const WtMeshingBackendInfo &get_info() const noexcept override;
	bool is_available() const noexcept override;
	WtCellStatus mesh_regular_cell(
		const WtRegularCellInput &input,
		WtCellMesh &output,
		WtCellMeshingScratch &scratch
	) const noexcept override;
	WtCellStatus mesh_transition_cell(
		const WtTransitionCellInput &input,
		WtCellMesh &output,
		WtCellMeshingScratch &scratch
	) const noexcept override;

	const std::vector<WtRecordedMeshingCell> &records() const noexcept;
	bool overflowed() const noexcept;

private:
	const WtMeshingBackend &authority_;
	mutable std::vector<WtRecordedMeshingCell> records_;
	mutable bool overflowed_ = false;
};

class WtReplayMeshingBackend final : public WtMeshingBackend {
public:
	WtReplayMeshingBackend(
		const WtMeshingBackend &authority,
		const std::vector<WtReplayMeshingCell> &cells
	) noexcept;

	const WtMeshingBackendInfo &get_info() const noexcept override;
	bool is_available() const noexcept override;
	WtCellStatus mesh_regular_cell(
		const WtRegularCellInput &input,
		WtCellMesh &output,
		WtCellMeshingScratch &scratch
	) const noexcept override;
	WtCellStatus mesh_transition_cell(
		const WtTransitionCellInput &input,
		WtCellMesh &output,
		WtCellMeshingScratch &scratch
	) const noexcept override;

	bool complete() const noexcept;
	std::size_t consumed_cell_count() const noexcept;
	WtReplayMeshingFailure failure() const noexcept;

private:
	WtCellStatus fail(WtReplayMeshingFailure failure) const noexcept;

	const WtMeshingBackend &authority_;
	const std::vector<WtReplayMeshingCell> &cells_;
	mutable std::size_t next_cell_ = 0;
	mutable WtReplayMeshingFailure failure_ = WtReplayMeshingFailure::None;
};

std::uint16_t wt_regular_case_code(const WtRegularCellInput &input) noexcept;
std::uint16_t wt_transition_case_code(const WtTransitionCellInput &input) noexcept;
WtReplayMeshingCell wt_make_replay_meshing_cell(
	const WtRecordedMeshingCell &record
) noexcept;
const char *wt_replay_meshing_failure_name(WtReplayMeshingFailure failure) noexcept;

} // namespace world_transvoxel
