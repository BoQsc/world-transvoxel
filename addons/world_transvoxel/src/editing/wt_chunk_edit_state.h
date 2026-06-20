#pragma once

#include "editing/wt_edit_journal.h"
#include "storage/wt_chunk_page.h"

#include <cstddef>
#include <cstdint>

namespace world_transvoxel {

enum class WtChunkEditStatus : std::uint8_t {
	Ok,
	NotInitialized,
	InvalidPage,
	SourceRevisionMismatch,
	InvalidCommand,
	WorldRevisionMismatch,
	SequenceMismatch,
	NonFiniteResult,
};

class WtChunkEditState final : public WtEditReplaySink {
public:
	WtChunkEditStatus initialize(
		WtChunkPage page,
		std::uint64_t expected_source_revision,
		std::uint64_t initial_world_revision
	);

	WtChunkEditStatus apply_command(
		const WtEditCommand &command
	) noexcept;

	bool apply(const WtEditCommand &command) noexcept override;

	bool initialized() const noexcept;
	const WtChunkPage &page() const noexcept;
	std::uint64_t current_world_revision() const noexcept;
	std::uint32_t next_sequence() const noexcept;
	std::size_t changed_sample_count() const noexcept;
	WtChunkEditStatus last_status() const noexcept;

private:
	WtChunkPage page_;
	bool initialized_ = false;
	std::uint64_t current_world_revision_ = 0;
	std::uint32_t next_sequence_ = 0;
	std::size_t changed_sample_count_ = 0;
	WtChunkEditStatus last_status_ = WtChunkEditStatus::NotInitialized;
};

} // namespace world_transvoxel
