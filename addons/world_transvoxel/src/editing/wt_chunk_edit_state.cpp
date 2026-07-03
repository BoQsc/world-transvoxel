#include "editing/wt_chunk_edit_state.h"

#include <cmath>
#include <limits>
#include <utility>

namespace world_transvoxel {
namespace {

using WideSigned = __int128_t;
using WideUnsigned = __uint128_t;

bool valid_page(const WtChunkPage &page) noexcept {
	return wt_is_valid_chunk_key(page.metadata.key) &&
		page.metadata.sample_minimum == -1 &&
		page.metadata.sample_maximum == 17 &&
		page.metadata.dimension_x == kWtChunkMeshingSamplesPerAxis &&
		page.metadata.dimension_y == kWtChunkMeshingSamplesPerAxis &&
		page.metadata.dimension_z == kWtChunkMeshingSamplesPerAxis &&
		page.metadata.density_encoding == WtDensityEncoding::Float32 &&
		page.metadata.material_encoding == WtMaterialEncoding::Uint16 &&
		page.metadata.sample_count == kWtChunkPageSampleCount &&
		page.metadata.cell_spacing == static_cast<std::uint64_t>(
			wt_lod_cell_size(page.metadata.key.lod)
		) &&
		page.samples.size() == kWtChunkPageSampleCount;
}

bool point_in_bounds(
	const WtGridPoint &point,
	const WtEditBounds &bounds
) noexcept {
	return point.x >= bounds.minimum.x && point.x <= bounds.maximum.x &&
		point.y >= bounds.minimum.y && point.y <= bounds.maximum.y &&
		point.z >= bounds.minimum.z && point.z <= bounds.maximum.z;
}

bool point_in_sphere(
	const WtGridPoint &point,
	const WtEditSphere &sphere
) noexcept {
	const WideSigned coordinates[3] = {
		static_cast<WideSigned>(point.x) * kWtEditCoordinateScale,
		static_cast<WideSigned>(point.y) * kWtEditCoordinateScale,
		static_cast<WideSigned>(point.z) * kWtEditCoordinateScale,
	};
	const WideSigned centers[3] = {
		sphere.center_x_q16,
		sphere.center_y_q16,
		sphere.center_z_q16,
	};
	const WideSigned radius = static_cast<WideSigned>(sphere.radius_q16);
	WideUnsigned squared_distance = 0;
	for (std::size_t axis = 0; axis < 3; ++axis) {
		const WideSigned delta = coordinates[axis] - centers[axis];
		if (delta < -radius || delta > radius) {
			return false;
		}
		const WideUnsigned magnitude = static_cast<WideUnsigned>(
			delta < 0 ? -delta : delta
		);
		squared_distance += magnitude * magnitude;
	}
	const WideUnsigned unsigned_radius =
		static_cast<WideUnsigned>(sphere.radius_q16);
	return squared_distance <= unsigned_radius * unsigned_radius;
}

bool point_in_box(
	const WtGridPoint &point,
	const WtEditBox &box
) noexcept {
	const WideSigned x =
		static_cast<WideSigned>(point.x) * kWtEditCoordinateScale;
	const WideSigned y =
		static_cast<WideSigned>(point.y) * kWtEditCoordinateScale;
	const WideSigned z =
		static_cast<WideSigned>(point.z) * kWtEditCoordinateScale;
	return x >= box.minimum_x_q16 && x <= box.maximum_x_q16 &&
		y >= box.minimum_y_q16 && y <= box.maximum_y_q16 &&
		z >= box.minimum_z_q16 && z <= box.maximum_z_q16;
}

WideUnsigned unsigned_magnitude(WideSigned value) noexcept {
	return static_cast<WideUnsigned>(value < 0 ? -value : value);
}

WideUnsigned squared_distance_to_interval(
	WideSigned value,
	WideSigned minimum,
	WideSigned maximum
) noexcept {
	if (value < minimum) {
		const WideUnsigned delta = unsigned_magnitude(minimum - value);
		return delta * delta;
	}
	if (value > maximum) {
		const WideUnsigned delta = unsigned_magnitude(value - maximum);
		return delta * delta;
	}
	return 0;
}

bool sphere_intersects_sample_footprint(
	const WtGridPoint &point,
	const WtEditSphere &sphere,
	std::int64_t spacing
) noexcept {
	if (spacing <= 1) {
		return point_in_sphere(point, sphere);
	}
	const WideSigned footprint_half =
		static_cast<WideSigned>(spacing) *
		static_cast<WideSigned>(kWtEditCoordinateScale) / 2;
	const WideSigned coordinates[3] = {
		static_cast<WideSigned>(point.x) * kWtEditCoordinateScale,
		static_cast<WideSigned>(point.y) * kWtEditCoordinateScale,
		static_cast<WideSigned>(point.z) * kWtEditCoordinateScale,
	};
	const WideSigned centers[3] = {
		sphere.center_x_q16,
		sphere.center_y_q16,
		sphere.center_z_q16,
	};
	WideUnsigned squared_distance = 0;
	for (std::size_t axis = 0; axis < 3; ++axis) {
		squared_distance += squared_distance_to_interval(
			centers[axis],
			coordinates[axis] - footprint_half,
			coordinates[axis] + footprint_half
		);
	}
	const WideUnsigned radius =
		static_cast<WideUnsigned>(sphere.radius_q16);
	return squared_distance <= radius * radius;
}

bool box_intersects_sample_footprint(
	const WtGridPoint &point,
	const WtEditBox &box,
	std::int64_t spacing
) noexcept {
	if (spacing <= 1) {
		return point_in_box(point, box);
	}
	const WideSigned footprint_half =
		static_cast<WideSigned>(spacing) *
		static_cast<WideSigned>(kWtEditCoordinateScale) / 2;
	const WideSigned x =
		static_cast<WideSigned>(point.x) * kWtEditCoordinateScale;
	const WideSigned y =
		static_cast<WideSigned>(point.y) * kWtEditCoordinateScale;
	const WideSigned z =
		static_cast<WideSigned>(point.z) * kWtEditCoordinateScale;
	return x >= static_cast<WideSigned>(box.minimum_x_q16) - footprint_half &&
		x <= static_cast<WideSigned>(box.maximum_x_q16) + footprint_half &&
		y >= static_cast<WideSigned>(box.minimum_y_q16) - footprint_half &&
		y <= static_cast<WideSigned>(box.maximum_y_q16) + footprint_half &&
		z >= static_cast<WideSigned>(box.minimum_z_q16) - footprint_half &&
		z <= static_cast<WideSigned>(box.maximum_z_q16) + footprint_half;
}

bool contains(
	const WtEditCommand &command,
	const WtGridPoint &point,
	std::int64_t spacing
) noexcept {
	if (!point_in_bounds(point, command.bounds)) {
		if (spacing <= 1) {
			return false;
		}
	}
	return command.shape == WtEditShape::Sphere ?
		sphere_intersects_sample_footprint(point, command.sphere, spacing) :
		box_intersects_sample_footprint(point, command.box, spacing);
}

WtGridPoint sample_point(
	const WtChunkPageMetadata &metadata,
	int x,
	int y,
	int z
) noexcept {
	const WtChunkBounds bounds = wt_chunk_bounds(metadata.key);
	const std::int64_t spacing =
		static_cast<std::int64_t>(metadata.cell_spacing);
	return {
		bounds.minimum.x + static_cast<std::int64_t>(x) * spacing,
		bounds.minimum.y + static_cast<std::int64_t>(y) * spacing,
		bounds.minimum.z + static_cast<std::int64_t>(z) * spacing,
	};
}

bool may_intersect_page(
	const WtChunkPageMetadata &metadata,
	const WtEditBounds &bounds
) noexcept {
	const WtChunkBounds chunk = wt_chunk_bounds(metadata.key);
	const std::int64_t spacing =
		static_cast<std::int64_t>(metadata.cell_spacing);
	const WtEditBounds page_bounds = {
		{
			chunk.minimum.x - spacing,
			chunk.minimum.y - spacing,
			chunk.minimum.z - spacing,
		},
		{
			chunk.maximum.x + spacing,
			chunk.maximum.y + spacing,
			chunk.maximum.z + spacing,
		},
	};
	return bounds.maximum.x >= page_bounds.minimum.x &&
		bounds.minimum.x <= page_bounds.maximum.x &&
		bounds.maximum.y >= page_bounds.minimum.y &&
		bounds.minimum.y <= page_bounds.maximum.y &&
		bounds.maximum.z >= page_bounds.minimum.z &&
		bounds.minimum.z <= page_bounds.maximum.z;
}

bool density_result_is_finite(
	const WtChunkPage &page,
	const WtEditCommand &command
) noexcept {
	if (command.operation != WtEditOperation::AddDensity ||
		!may_intersect_page(page.metadata, command.bounds)) {
		return true;
	}
	const std::int64_t spacing =
		static_cast<std::int64_t>(page.metadata.cell_spacing);
	std::size_t index = 0;
	for (int z = -1; z <= 17; ++z) {
		for (int y = -1; y <= 17; ++y) {
			for (int x = -1; x <= 17; ++x, ++index) {
				if (contains(
						command,
						sample_point(page.metadata, x, y, z),
						spacing
					) &&
					!std::isfinite(
						page.samples[index].density +
						command.density_value
					)) {
					return false;
				}
			}
		}
	}
	return true;
}

std::size_t apply_values(
	WtChunkPage &page,
	const WtEditCommand &command
) noexcept {
	if (!may_intersect_page(page.metadata, command.bounds)) {
		return 0;
	}
	const std::int64_t spacing =
		static_cast<std::int64_t>(page.metadata.cell_spacing);
	std::size_t changed = 0;
	std::size_t index = 0;
	for (int z = -1; z <= 17; ++z) {
		for (int y = -1; y <= 17; ++y) {
			for (int x = -1; x <= 17; ++x, ++index) {
				if (!contains(
						command,
						sample_point(page.metadata, x, y, z),
						spacing
					)) {
					continue;
				}
				WtScalarSample &sample = page.samples[index];
				const float previous_density = sample.density;
				const std::uint16_t previous_material = sample.material;
				if (command.operation == WtEditOperation::AddDensity) {
					sample.density += command.density_value;
				} else if (command.operation == WtEditOperation::SetDensity) {
					sample.density = command.density_value;
				} else {
					sample.material = command.material;
				}
				if (sample.density != previous_density ||
					sample.material != previous_material) {
					++changed;
				}
			}
		}
	}
	return changed;
}

} // namespace

WtChunkEditStatus WtChunkEditState::initialize(
	WtChunkPage page,
	std::uint64_t expected_source_revision,
	std::uint64_t initial_world_revision
) {
	initialized_ = false;
	page_ = {};
	current_world_revision_ = 0;
	next_sequence_ = 0;
	changed_sample_count_ = 0;
	if (!valid_page(page)) {
		last_status_ = WtChunkEditStatus::InvalidPage;
		return last_status_;
	}
	for (const WtScalarSample &sample : page.samples) {
		if (!std::isfinite(sample.density)) {
			last_status_ = WtChunkEditStatus::InvalidPage;
			return last_status_;
		}
	}
	if (page.metadata.source_revision != expected_source_revision) {
		last_status_ = WtChunkEditStatus::SourceRevisionMismatch;
		return last_status_;
	}
	page_ = std::move(page);
	initialized_ = true;
	current_world_revision_ = initial_world_revision;
	last_status_ = WtChunkEditStatus::Ok;
	return last_status_;
}

WtChunkEditStatus WtChunkEditState::apply_command(
	const WtEditCommand &command
) noexcept {
	if (!initialized_) {
		last_status_ = WtChunkEditStatus::NotInitialized;
		return last_status_;
	}
	if (!wt_is_valid_edit_command(command)) {
		last_status_ = WtChunkEditStatus::InvalidCommand;
		return last_status_;
	}
	if (command.world_revision == current_world_revision_ + 1 &&
		current_world_revision_ != std::numeric_limits<std::uint64_t>::max()) {
		if (command.sequence != 0) {
			last_status_ = WtChunkEditStatus::SequenceMismatch;
			return last_status_;
		}
	} else if (command.world_revision == current_world_revision_) {
		if (command.sequence != next_sequence_) {
			last_status_ = WtChunkEditStatus::SequenceMismatch;
			return last_status_;
		}
	} else {
		last_status_ = WtChunkEditStatus::WorldRevisionMismatch;
		return last_status_;
	}
	if (!density_result_is_finite(page_, command)) {
		last_status_ = WtChunkEditStatus::NonFiniteResult;
		return last_status_;
	}
	if (command.world_revision != current_world_revision_) {
		current_world_revision_ = command.world_revision;
		next_sequence_ = 0;
	}
	changed_sample_count_ += apply_values(page_, command);
	next_sequence_ = command.sequence + 1;
	last_status_ = WtChunkEditStatus::Ok;
	return last_status_;
}

bool WtChunkEditState::apply(const WtEditCommand &command) noexcept {
	return apply_command(command) == WtChunkEditStatus::Ok;
}

bool WtChunkEditState::initialized() const noexcept {
	return initialized_;
}

const WtChunkPage &WtChunkEditState::page() const noexcept {
	return page_;
}

std::uint64_t WtChunkEditState::current_world_revision() const noexcept {
	return current_world_revision_;
}

std::uint32_t WtChunkEditState::next_sequence() const noexcept {
	return next_sequence_;
}

std::size_t WtChunkEditState::changed_sample_count() const noexcept {
	return changed_sample_count_;
}

WtChunkEditStatus WtChunkEditState::last_status() const noexcept {
	return last_status_;
}

} // namespace world_transvoxel
