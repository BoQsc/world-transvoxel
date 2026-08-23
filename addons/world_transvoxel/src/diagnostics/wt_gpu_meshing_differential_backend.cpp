#include "diagnostics/wt_gpu_meshing_differential_backend.h"

#include <array>

namespace world_transvoxel {
namespace {

constexpr std::array<std::uint8_t, 9> kTransitionCaseBitSamples = {
	0, 1, 2, 5, 8, 7, 6, 3, 4
};

} // namespace

WtRecordingMeshingBackend::WtRecordingMeshingBackend(
	const WtMeshingBackend &authority
) : authority_(authority) {
	records_.reserve(kWtMaximumRecordedChunkCells);
}

const WtMeshingBackendInfo &WtRecordingMeshingBackend::get_info() const noexcept {
	return authority_.get_info();
}

bool WtRecordingMeshingBackend::is_available() const noexcept {
	return authority_.is_available();
}

WtCellStatus WtRecordingMeshingBackend::mesh_regular_cell(
	const WtRegularCellInput &input,
	WtCellMesh &output,
	WtCellMeshingScratch &scratch
) const noexcept {
	const WtCellStatus status = authority_.mesh_regular_cell(input, output, scratch);
	if (records_.size() >= kWtMaximumRecordedChunkCells) {
		overflowed_ = true;
		output.clear();
		return WtCellStatus::TopologyFailure;
	}
	WtRecordedMeshingCell record;
	record.type = WtRecordedCellType::Regular;
	record.regular_input = input;
	record.status = status;
	record.mesh = output;
	record.case_code = wt_regular_case_code(input);
	records_.push_back(record);
	return status;
}

WtCellStatus WtRecordingMeshingBackend::mesh_transition_cell(
	const WtTransitionCellInput &input,
	WtCellMesh &output,
	WtCellMeshingScratch &scratch
) const noexcept {
	const WtCellStatus status = authority_.mesh_transition_cell(input, output, scratch);
	if (records_.size() >= kWtMaximumRecordedChunkCells) {
		overflowed_ = true;
		output.clear();
		return WtCellStatus::TopologyFailure;
	}
	WtRecordedMeshingCell record;
	record.type = WtRecordedCellType::Transition;
	record.transition_input = input;
	record.status = status;
	record.mesh = output;
	record.case_code = wt_transition_case_code(input);
	records_.push_back(record);
	return status;
}

const std::vector<WtRecordedMeshingCell> &
WtRecordingMeshingBackend::records() const noexcept {
	return records_;
}

bool WtRecordingMeshingBackend::overflowed() const noexcept {
	return overflowed_;
}

WtReplayMeshingBackend::WtReplayMeshingBackend(
	const WtMeshingBackend &authority,
	const std::vector<WtReplayMeshingCell> &cells
) noexcept : authority_(authority), cells_(cells) {
}

const WtMeshingBackendInfo &WtReplayMeshingBackend::get_info() const noexcept {
	return authority_.get_info();
}

bool WtReplayMeshingBackend::is_available() const noexcept {
	return authority_.is_available();
}

WtCellStatus WtReplayMeshingBackend::mesh_regular_cell(
	const WtRegularCellInput &input,
	WtCellMesh &output,
	WtCellMeshingScratch &scratch
) const noexcept {
	if (next_cell_ >= cells_.size()) {
		return fail(WtReplayMeshingFailure::CellSequenceExhausted);
	}
	const WtReplayMeshingCell &cell = cells_[next_cell_++];
	if (cell.type != WtRecordedCellType::Regular) {
		return fail(WtReplayMeshingFailure::CellTypeMismatch);
	}
	if (cell.case_code != wt_regular_case_code(input)) {
		return fail(WtReplayMeshingFailure::CaseCodeMismatch);
	}
	WtCellMesh authority_output;
	const WtCellStatus authority_status =
		authority_.mesh_regular_cell(input, authority_output, scratch);
	if (authority_status != cell.status) {
		return fail(WtReplayMeshingFailure::AuthorityStatusMismatch);
	}
	if (cell.status == WtCellStatus::Empty) {
		output.clear();
		return WtCellStatus::Empty;
	}
	if (cell.status != WtCellStatus::Ok) {
		return fail(WtReplayMeshingFailure::UnsupportedStatus);
	}
	output = cell.mesh;
	return WtCellStatus::Ok;
}

WtCellStatus WtReplayMeshingBackend::mesh_transition_cell(
	const WtTransitionCellInput &input,
	WtCellMesh &output,
	WtCellMeshingScratch &scratch
) const noexcept {
	if (next_cell_ >= cells_.size()) {
		return fail(WtReplayMeshingFailure::CellSequenceExhausted);
	}
	const WtReplayMeshingCell &cell = cells_[next_cell_++];
	if (cell.type != WtRecordedCellType::Transition) {
		return fail(WtReplayMeshingFailure::CellTypeMismatch);
	}
	if (cell.orientation != input.orientation) {
		return fail(WtReplayMeshingFailure::OrientationMismatch);
	}
	if (cell.case_code != wt_transition_case_code(input)) {
		return fail(WtReplayMeshingFailure::CaseCodeMismatch);
	}
	WtCellMesh authority_output;
	const WtCellStatus authority_status =
		authority_.mesh_transition_cell(input, authority_output, scratch);
	if (authority_status != cell.status) {
		return fail(WtReplayMeshingFailure::AuthorityStatusMismatch);
	}
	if (cell.status == WtCellStatus::Empty) {
		output.clear();
		return WtCellStatus::Empty;
	}
	if (cell.status != WtCellStatus::Ok) {
		return fail(WtReplayMeshingFailure::UnsupportedStatus);
	}
	output = cell.mesh;
	return WtCellStatus::Ok;
}

bool WtReplayMeshingBackend::complete() const noexcept {
	return failure_ == WtReplayMeshingFailure::None &&
		next_cell_ == cells_.size();
}

std::size_t WtReplayMeshingBackend::consumed_cell_count() const noexcept {
	return next_cell_;
}

WtReplayMeshingFailure WtReplayMeshingBackend::failure() const noexcept {
	return failure_;
}

WtCellStatus WtReplayMeshingBackend::fail(
	WtReplayMeshingFailure failure
) const noexcept {
	if (failure_ == WtReplayMeshingFailure::None) {
		failure_ = failure;
	}
	return WtCellStatus::TopologyFailure;
}

std::uint16_t wt_regular_case_code(const WtRegularCellInput &input) noexcept {
	std::uint16_t result = 0;
	for (unsigned int index = 0; index < kWtRegularSampleCount; ++index) {
		if (input.samples[index].density < input.isovalue) {
			result |= static_cast<std::uint16_t>(1U << index);
		}
	}
	return result;
}

std::uint16_t wt_transition_case_code(const WtTransitionCellInput &input) noexcept {
	std::uint16_t result = 0;
	for (unsigned int bit = 0; bit < kTransitionCaseBitSamples.size(); ++bit) {
		if (input.samples[kTransitionCaseBitSamples[bit]].density < input.isovalue) {
			result |= static_cast<std::uint16_t>(1U << bit);
		}
	}
	return result;
}

WtReplayMeshingCell wt_make_replay_meshing_cell(
	const WtRecordedMeshingCell &record
) noexcept {
	WtReplayMeshingCell result;
	result.type = record.type;
	result.orientation = record.transition_input.orientation;
	result.status = record.status;
	result.mesh = record.mesh;
	result.case_code = record.case_code;
	return result;
}

const char *wt_replay_meshing_failure_name(
	WtReplayMeshingFailure failure
) noexcept {
	switch (failure) {
		case WtReplayMeshingFailure::None: return "None";
		case WtReplayMeshingFailure::CellSequenceExhausted: return "CellSequenceExhausted";
		case WtReplayMeshingFailure::CellTypeMismatch: return "CellTypeMismatch";
		case WtReplayMeshingFailure::CaseCodeMismatch: return "CaseCodeMismatch";
		case WtReplayMeshingFailure::OrientationMismatch: return "OrientationMismatch";
		case WtReplayMeshingFailure::AuthorityStatusMismatch: return "AuthorityStatusMismatch";
		case WtReplayMeshingFailure::UnsupportedStatus: return "UnsupportedStatus";
	}
	return "Unknown";
}

} // namespace world_transvoxel
