#include "api/world_transvoxel_terrain.h"

#include <godot_cpp/core/class_db.hpp>

namespace world_transvoxel {

void WorldTransvoxelTerrain::bind_edit_methods() {
	godot::ClassDB::bind_method(
		godot::D_METHOD("begin_edit_transaction", "author_id"),
		&WorldTransvoxelTerrain::begin_edit_transaction,
		DEFVAL(0)
	);
	godot::ClassDB::bind_method(
		godot::D_METHOD("commit_edit_transaction", "transaction"),
		&WorldTransvoxelTerrain::commit_edit_transaction
	);
	ADD_SIGNAL(godot::MethodInfo(
		"edit_committed",
		godot::PropertyInfo(godot::Variant::INT, "world_revision")
	));
	ADD_SIGNAL(godot::MethodInfo(
		"edit_failed",
		godot::PropertyInfo(godot::Variant::STRING, "error")
	));
}

godot::Ref<WorldTransvoxelEditTransaction>
WorldTransvoxelTerrain::begin_edit_transaction(std::int64_t author_id) {
	godot::Ref<WorldTransvoxelEditTransaction> transaction;
	if (!is_world_running() || author_id < 0 ||
		get_world_source_revision() <= 0) {
		synchronous_world_error_ =
			"running world and non-negative author id are required";
		return transaction;
	}
	transaction.instantiate();
	transaction->initialize(
		static_cast<std::uint64_t>(get_world_source_revision()),
		static_cast<std::uint64_t>(get_world_revision()),
		static_cast<std::uint64_t>(author_id)
	);
	synchronous_world_error_ = transaction->get_error();
	return transaction;
}

bool WorldTransvoxelTerrain::commit_edit_transaction(
	const godot::Ref<WorldTransvoxelEditTransaction> &transaction
) {
	if (!is_world_running() || transaction.is_null() ||
		!transaction->initialized_ || transaction->submitted_ ||
		transaction->transaction_.commands.empty()) {
		synchronous_world_error_ = transaction.is_valid() ?
			(transaction->submitted_ ?
				godot::String("transaction is already submitted") :
				godot::String("transaction is invalid or empty")) :
			godot::String("transaction is required");
		return false;
	}
	const WtReadOnlyRuntimeStatus status = lifecycle_->submit_edit(
		transaction->transaction_
	);
	synchronous_world_error_ = wt_read_only_runtime_status_message(status);
	if (status != WtReadOnlyRuntimeStatus::Ok) return false;
	transaction->submitted_ = true;
	transaction->error_ = "ok";
	return true;
}

} // namespace world_transvoxel
