#include "editing/wt_edit_transaction.h"

#include "storage/wt_binary_io.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace world_transvoxel {
namespace {

constexpr std::size_t kSpherePayloadSize = 32;
constexpr std::size_t kBoxPayloadSize = 48;

std::size_t payload_size(WtEditShape shape) noexcept {
	return shape == WtEditShape::Sphere ?
		kSpherePayloadSize :
		(shape == WtEditShape::AxisAlignedBox ? kBoxPayloadSize : 0);
}

bool valid_transaction_header(const WtEditTransaction &transaction) noexcept {
	return !wt_is_zero_id(transaction.transaction_id) &&
		transaction.base_revision != std::numeric_limits<std::uint64_t>::max() &&
		transaction.committed_revision == transaction.base_revision + 1 &&
		!transaction.commands.empty() &&
		transaction.commands.size() <= kWtMaximumEditCommandCount;
}

WtEditTransactionStatus encode_header(
	const WtEditTransaction &transaction,
	std::vector<std::uint8_t> &output
) {
	WtBinaryWriter writer(kWtEditHeaderSize);
	if (writer.write_u16(kWtEditSchemaMajor) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtEditSchemaMinor) != WtBinaryStatus::Ok ||
		writer.write_bytes(
			transaction.transaction_id.data(),
			transaction.transaction_id.size()
		) != WtBinaryStatus::Ok ||
		writer.write_u64(transaction.source_revision) != WtBinaryStatus::Ok ||
		writer.write_u64(transaction.base_revision) != WtBinaryStatus::Ok ||
		writer.write_u64(transaction.committed_revision) != WtBinaryStatus::Ok ||
		writer.write_u64(transaction.author_id) != WtBinaryStatus::Ok ||
		writer.write_u32(static_cast<std::uint32_t>(
			transaction.commands.size()
		)) != WtBinaryStatus::Ok ||
		writer.write_u32(0) != WtBinaryStatus::Ok ||
		writer.write_u32(0) != WtBinaryStatus::Ok) {
		return WtEditTransactionStatus::CapacityExceeded;
	}
	output = writer.take_bytes();
	return output.size() == kWtEditHeaderSize ?
		WtEditTransactionStatus::Ok :
		WtEditTransactionStatus::CapacityExceeded;
}

WtEditTransactionStatus encode_command(
	const WtEditCommand &command,
	std::vector<std::uint8_t> &output
) {
	const std::size_t shape_payload_size = payload_size(command.shape);
	const std::size_t record_size =
		kWtEditCommandHeaderSize +
		shape_payload_size +
		kWtEditRecordHashSize;
	WtBinaryWriter writer(record_size);
	if (writer.write_u32(static_cast<std::uint32_t>(record_size)) !=
			WtBinaryStatus::Ok ||
		writer.write_u16(kWtEditSchemaMajor) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtEditSchemaMinor) != WtBinaryStatus::Ok ||
		writer.write_bytes(command.command_id.data(), command.command_id.size()) !=
			WtBinaryStatus::Ok ||
		writer.write_u32(command.sequence) != WtBinaryStatus::Ok ||
		writer.write_u64(command.world_revision) != WtBinaryStatus::Ok ||
		writer.write_u16(static_cast<std::uint16_t>(command.operation)) !=
			WtBinaryStatus::Ok ||
		writer.write_u16(static_cast<std::uint16_t>(command.shape)) !=
			WtBinaryStatus::Ok ||
		writer.write_u32(command.flags) != WtBinaryStatus::Ok ||
		writer.write_f32(command.density_value) != WtBinaryStatus::Ok ||
		writer.write_u16(command.material) != WtBinaryStatus::Ok ||
		writer.write_u16(0) != WtBinaryStatus::Ok ||
		writer.write_i64(command.bounds.minimum.x) != WtBinaryStatus::Ok ||
		writer.write_i64(command.bounds.minimum.y) != WtBinaryStatus::Ok ||
		writer.write_i64(command.bounds.minimum.z) != WtBinaryStatus::Ok ||
		writer.write_i64(command.bounds.maximum.x) != WtBinaryStatus::Ok ||
		writer.write_i64(command.bounds.maximum.y) != WtBinaryStatus::Ok ||
		writer.write_i64(command.bounds.maximum.z) != WtBinaryStatus::Ok ||
		writer.write_u32(static_cast<std::uint32_t>(shape_payload_size)) !=
			WtBinaryStatus::Ok ||
		writer.write_u32(command.smooth_radius_q16) != WtBinaryStatus::Ok) {
		return WtEditTransactionStatus::CapacityExceeded;
	}
	if (command.shape == WtEditShape::Sphere) {
		if (writer.write_i64(command.sphere.center_x_q16) != WtBinaryStatus::Ok ||
			writer.write_i64(command.sphere.center_y_q16) != WtBinaryStatus::Ok ||
			writer.write_i64(command.sphere.center_z_q16) != WtBinaryStatus::Ok ||
			writer.write_u64(command.sphere.radius_q16) != WtBinaryStatus::Ok) {
			return WtEditTransactionStatus::CapacityExceeded;
		}
	} else {
		if (writer.write_i64(command.box.minimum_x_q16) != WtBinaryStatus::Ok ||
			writer.write_i64(command.box.minimum_y_q16) != WtBinaryStatus::Ok ||
			writer.write_i64(command.box.minimum_z_q16) != WtBinaryStatus::Ok ||
			writer.write_i64(command.box.maximum_x_q16) != WtBinaryStatus::Ok ||
			writer.write_i64(command.box.maximum_y_q16) != WtBinaryStatus::Ok ||
			writer.write_i64(command.box.maximum_z_q16) != WtBinaryStatus::Ok) {
			return WtEditTransactionStatus::CapacityExceeded;
		}
	}
	const WtHash256 hash = wt_sha256(writer.bytes().data(), writer.bytes().size());
	if (writer.write_bytes(hash.data(), hash.size()) != WtBinaryStatus::Ok) {
		return WtEditTransactionStatus::CapacityExceeded;
	}
	output = writer.take_bytes();
	return WtEditTransactionStatus::Ok;
}

WtEditTransactionStatus encode_commands(
	const std::vector<WtEditCommand> &commands,
	std::vector<std::uint8_t> &output
) {
	std::size_t total_size = kWtEditCommandSectionHeaderSize;
	for (const WtEditCommand &command : commands) {
		total_size += kWtEditCommandHeaderSize +
			payload_size(command.shape) +
			kWtEditRecordHashSize;
	}
	if (total_size > kWtMaximumSectionSize) {
		return WtEditTransactionStatus::CapacityExceeded;
	}
	WtBinaryWriter writer(total_size);
	if (writer.write_u16(kWtEditSchemaMajor) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtEditSchemaMinor) != WtBinaryStatus::Ok ||
		writer.write_u32(static_cast<std::uint32_t>(commands.size())) !=
			WtBinaryStatus::Ok) {
		return WtEditTransactionStatus::CapacityExceeded;
	}
	for (const WtEditCommand &command : commands) {
		std::vector<std::uint8_t> record;
		const WtEditTransactionStatus status = encode_command(command, record);
		if (status != WtEditTransactionStatus::Ok) {
			return status;
		}
		if (writer.write_bytes(record.data(), record.size()) != WtBinaryStatus::Ok) {
			return WtEditTransactionStatus::CapacityExceeded;
		}
	}
	output = writer.take_bytes();
	return WtEditTransactionStatus::Ok;
}

WtEditTransactionStatus encode_commit(
	const WtEditTransaction &transaction,
	const std::vector<std::uint8_t> &commands,
	std::vector<std::uint8_t> &output
) {
	WtBinaryWriter writer(kWtEditCommitSize);
	const WtHash256 commands_hash = wt_sha256(commands.data(), commands.size());
	if (writer.write_u16(kWtEditSchemaMajor) != WtBinaryStatus::Ok ||
		writer.write_u16(kWtEditSchemaMinor) != WtBinaryStatus::Ok ||
		writer.write_bytes(
			transaction.transaction_id.data(),
			transaction.transaction_id.size()
		) != WtBinaryStatus::Ok ||
		writer.write_u64(transaction.committed_revision) != WtBinaryStatus::Ok ||
		writer.write_u32(static_cast<std::uint32_t>(
			transaction.commands.size()
		)) != WtBinaryStatus::Ok ||
		writer.write_u32(0) != WtBinaryStatus::Ok ||
		writer.write_bytes(commands_hash.data(), commands_hash.size()) !=
			WtBinaryStatus::Ok) {
		return WtEditTransactionStatus::CapacityExceeded;
	}
	output = writer.take_bytes();
	return output.size() == kWtEditCommitSize ?
		WtEditTransactionStatus::Ok :
		WtEditTransactionStatus::CapacityExceeded;
}

bool decode_header(
	WtByteView bytes,
	WtEditTransaction &transaction,
	std::uint32_t &command_count
) {
	if (bytes.size != kWtEditHeaderSize) return false;
	WtBinaryReader reader(bytes);
	std::uint16_t major = 0;
	std::uint16_t minor = 0;
	std::uint32_t reserved_a = 0;
	std::uint32_t reserved_b = 0;
	WtByteView id;
	if (reader.read_u16(major) != WtBinaryStatus::Ok ||
		reader.read_u16(minor) != WtBinaryStatus::Ok ||
		reader.read_bytes(transaction.transaction_id.size(), id) !=
			WtBinaryStatus::Ok ||
		reader.read_u64(transaction.source_revision) != WtBinaryStatus::Ok ||
		reader.read_u64(transaction.base_revision) != WtBinaryStatus::Ok ||
		reader.read_u64(transaction.committed_revision) != WtBinaryStatus::Ok ||
		reader.read_u64(transaction.author_id) != WtBinaryStatus::Ok ||
		reader.read_u32(command_count) != WtBinaryStatus::Ok ||
		reader.read_u32(reserved_a) != WtBinaryStatus::Ok ||
		reader.read_u32(reserved_b) != WtBinaryStatus::Ok) {
		return false;
	}
	std::copy(id.data, id.data + id.size, transaction.transaction_id.begin());
	return major == kWtEditSchemaMajor &&
		minor <= kWtEditSchemaMinor &&
		reserved_a == 0 && reserved_b == 0 &&
		command_count > 0 &&
		command_count <= kWtMaximumEditCommandCount &&
		reader.remaining() == 0;
}

bool decode_command(WtByteView bytes, WtEditCommand &command) {
	if (bytes.size < kWtEditCommandHeaderSize + kWtEditRecordHashSize) {
		return false;
	}
	const std::size_t content_size = bytes.size - kWtEditRecordHashSize;
	WtHash256 stored_hash{};
	std::copy(
		bytes.data + content_size,
		bytes.data + bytes.size,
		stored_hash.begin()
	);
	if (wt_sha256(bytes.data, content_size) != stored_hash) {
		return false;
	}
	WtBinaryReader reader({ bytes.data, content_size });
	std::uint32_t record_size = 0;
	std::uint16_t major = 0;
	std::uint16_t minor = 0;
	std::uint16_t operation = 0;
	std::uint16_t shape = 0;
	std::uint16_t reserved_short = 0;
	std::uint32_t shape_payload_size = 0;
	std::uint32_t smooth_radius_q16 = 0;
	WtByteView id;
	if (reader.read_u32(record_size) != WtBinaryStatus::Ok ||
		reader.read_u16(major) != WtBinaryStatus::Ok ||
		reader.read_u16(minor) != WtBinaryStatus::Ok ||
		reader.read_bytes(command.command_id.size(), id) != WtBinaryStatus::Ok ||
		reader.read_u32(command.sequence) != WtBinaryStatus::Ok ||
		reader.read_u64(command.world_revision) != WtBinaryStatus::Ok ||
		reader.read_u16(operation) != WtBinaryStatus::Ok ||
		reader.read_u16(shape) != WtBinaryStatus::Ok ||
		reader.read_u32(command.flags) != WtBinaryStatus::Ok ||
		reader.read_f32(command.density_value) != WtBinaryStatus::Ok ||
		reader.read_u16(command.material) != WtBinaryStatus::Ok ||
		reader.read_u16(reserved_short) != WtBinaryStatus::Ok ||
		reader.read_i64(command.bounds.minimum.x) != WtBinaryStatus::Ok ||
		reader.read_i64(command.bounds.minimum.y) != WtBinaryStatus::Ok ||
		reader.read_i64(command.bounds.minimum.z) != WtBinaryStatus::Ok ||
		reader.read_i64(command.bounds.maximum.x) != WtBinaryStatus::Ok ||
		reader.read_i64(command.bounds.maximum.y) != WtBinaryStatus::Ok ||
		reader.read_i64(command.bounds.maximum.z) != WtBinaryStatus::Ok ||
		reader.read_u32(shape_payload_size) != WtBinaryStatus::Ok ||
		reader.read_u32(smooth_radius_q16) != WtBinaryStatus::Ok) {
		return false;
	}
	std::copy(id.data, id.data + id.size, command.command_id.begin());
	command.operation = static_cast<WtEditOperation>(operation);
	command.shape = static_cast<WtEditShape>(shape);
	command.smooth_radius_q16 = minor >= 2 ? smooth_radius_q16 : 0;
	if (record_size != bytes.size ||
		major != kWtEditSchemaMajor ||
		minor > kWtEditSchemaMinor ||
		(minor == 0 && command.operation == WtEditOperation::SdfConstruct &&
			command.material != 0) ||
		reserved_short != 0 || (minor < 2 && smooth_radius_q16 != 0) ||
		shape_payload_size != payload_size(command.shape) ||
		reader.remaining() != shape_payload_size) {
		return false;
	}
	if (command.shape == WtEditShape::Sphere) {
		if (reader.read_i64(command.sphere.center_x_q16) != WtBinaryStatus::Ok ||
			reader.read_i64(command.sphere.center_y_q16) != WtBinaryStatus::Ok ||
			reader.read_i64(command.sphere.center_z_q16) != WtBinaryStatus::Ok ||
			reader.read_u64(command.sphere.radius_q16) != WtBinaryStatus::Ok) {
			return false;
		}
	} else if (command.shape == WtEditShape::AxisAlignedBox) {
		if (reader.read_i64(command.box.minimum_x_q16) != WtBinaryStatus::Ok ||
			reader.read_i64(command.box.minimum_y_q16) != WtBinaryStatus::Ok ||
			reader.read_i64(command.box.minimum_z_q16) != WtBinaryStatus::Ok ||
			reader.read_i64(command.box.maximum_x_q16) != WtBinaryStatus::Ok ||
			reader.read_i64(command.box.maximum_y_q16) != WtBinaryStatus::Ok ||
			reader.read_i64(command.box.maximum_z_q16) != WtBinaryStatus::Ok) {
			return false;
		}
	} else {
		return false;
	}
	return reader.remaining() == 0 && wt_is_valid_edit_command(command);
}

bool decode_commands(
	WtByteView bytes,
	std::uint32_t expected_count,
	std::vector<WtEditCommand> &output
) {
	WtBinaryReader reader(bytes);
	std::uint16_t major = 0;
	std::uint16_t minor = 0;
	std::uint32_t count = 0;
	if (reader.read_u16(major) != WtBinaryStatus::Ok ||
		reader.read_u16(minor) != WtBinaryStatus::Ok ||
		reader.read_u32(count) != WtBinaryStatus::Ok ||
		major != kWtEditSchemaMajor ||
		minor > kWtEditSchemaMinor ||
		count != expected_count) {
		return false;
	}
	output.reserve(count);
	for (std::uint32_t index = 0; index < count; ++index) {
		const std::size_t start = reader.position();
		std::uint32_t record_size = 0;
		if (reader.read_u32(record_size) != WtBinaryStatus::Ok ||
			record_size < kWtEditCommandHeaderSize + kWtEditRecordHashSize ||
			reader.seek(start) != WtBinaryStatus::Ok) {
			return false;
		}
		WtByteView record;
		if (reader.read_bytes(record_size, record) != WtBinaryStatus::Ok) {
			return false;
		}
		WtEditCommand command;
		if (!decode_command(record, command) ||
			command.sequence != index ||
			(!output.empty() &&
				output.back().world_revision != command.world_revision)) {
			return false;
		}
		output.push_back(command);
	}
	return reader.remaining() == 0;
}

bool decode_commit(
	WtByteView bytes,
	const WtEditTransaction &transaction,
	WtByteView command_bytes
) {
	if (bytes.size != kWtEditCommitSize) return false;
	WtBinaryReader reader(bytes);
	std::uint16_t major = 0;
	std::uint16_t minor = 0;
	WtId128 transaction_id{};
	std::uint64_t revision = 0;
	std::uint32_t count = 0;
	std::uint32_t reserved = 0;
	WtHash256 commands_hash{};
	WtByteView id;
	WtByteView hash;
	if (reader.read_u16(major) != WtBinaryStatus::Ok ||
		reader.read_u16(minor) != WtBinaryStatus::Ok ||
		reader.read_bytes(transaction_id.size(), id) != WtBinaryStatus::Ok ||
		reader.read_u64(revision) != WtBinaryStatus::Ok ||
		reader.read_u32(count) != WtBinaryStatus::Ok ||
		reader.read_u32(reserved) != WtBinaryStatus::Ok ||
		reader.read_bytes(commands_hash.size(), hash) != WtBinaryStatus::Ok) {
		return false;
	}
	std::copy(id.data, id.data + id.size, transaction_id.begin());
	std::copy(hash.data, hash.data + hash.size, commands_hash.begin());
	return major == kWtEditSchemaMajor &&
		minor <= kWtEditSchemaMinor &&
		transaction_id == transaction.transaction_id &&
		revision == transaction.committed_revision &&
		count == transaction.commands.size() &&
		reserved == 0 &&
		commands_hash == wt_sha256(command_bytes.data, command_bytes.size) &&
		reader.remaining() == 0;
}

} // namespace

WtEditTransactionStatus wt_write_edit_transaction(
	const WtEditTransaction &transaction,
	std::vector<std::uint8_t> &output
) {
	output.clear();
	if (!valid_transaction_header(transaction)) {
		return WtEditTransactionStatus::InvalidInput;
	}
	WtEditTransaction ordered = transaction;
	std::sort(
		ordered.commands.begin(),
		ordered.commands.end(),
		[](const WtEditCommand &left, const WtEditCommand &right) {
			return left.sequence < right.sequence;
		}
	);
	std::set<WtId128> command_ids;
	for (std::size_t index = 0; index < ordered.commands.size(); ++index) {
		const WtEditCommand &command = ordered.commands[index];
		if (!wt_is_valid_edit_command(command) ||
			command.sequence != index ||
			command.world_revision != ordered.committed_revision ||
			!command_ids.insert(command.command_id).second) {
			return WtEditTransactionStatus::InvalidInput;
		}
	}
	std::vector<std::uint8_t> header;
	std::vector<std::uint8_t> commands;
	std::vector<std::uint8_t> commit;
	WtEditTransactionStatus status = encode_header(ordered, header);
	if (status != WtEditTransactionStatus::Ok) return status;
	status = encode_commands(ordered.commands, commands);
	if (status != WtEditTransactionStatus::Ok) return status;
	status = encode_commit(ordered, commands, commit);
	if (status != WtEditTransactionStatus::Ok) return status;
	const std::vector<WtContainerSectionInput> sections = {
		{ kWtEditHeaderSection, 0, WtStorageCodec::None,
			{ header.data(), header.size() } },
		{ kWtEditCommandSection, 0, WtStorageCodec::None,
			{ commands.data(), commands.size() } },
		{ kWtEditCommitSection, 0, WtStorageCodec::None,
			{ commit.data(), commit.size() } },
	};
	return wt_write_container(
		kWtEditMagic,
		0,
		ordered.source_revision,
		sections,
		output
	) == WtContainerStatus::Ok ?
		WtEditTransactionStatus::Ok :
		WtEditTransactionStatus::ContainerFailure;
}

WtEditTransactionStatus wt_open_edit_transaction(
	WtByteView bytes,
	WtEditTransactionDocument &output
) {
	output = {};
	if (wt_read_container(bytes, kWtEditMagic, output.container) !=
		WtContainerStatus::Ok) {
		return WtEditTransactionStatus::ContainerFailure;
	}
	const WtContainerSection *header =
		output.container.find_section(kWtEditHeaderSection);
	const WtContainerSection *commands =
		output.container.find_section(kWtEditCommandSection);
	const WtContainerSection *commit =
		output.container.find_section(kWtEditCommitSection);
	if (output.container.sections.size() != 3 ||
		header == nullptr || commands == nullptr || commit == nullptr ||
		header->flags != 0 || commands->flags != 0 || commit->flags != 0) {
		output = {};
		return WtEditTransactionStatus::InvalidTransaction;
	}
	std::uint32_t command_count = 0;
	if (!decode_header(header->payload, output.transaction, command_count) ||
		output.transaction.source_revision !=
			output.container.header.source_revision ||
		!decode_commands(
			commands->payload,
			command_count,
			output.transaction.commands
		) ||
		!valid_transaction_header(output.transaction)) {
		output = {};
		return WtEditTransactionStatus::InvalidTransaction;
	}
	std::set<WtId128> command_ids;
	for (const WtEditCommand &command : output.transaction.commands) {
		if (command.world_revision != output.transaction.committed_revision ||
			!command_ids.insert(command.command_id).second) {
			output = {};
			return WtEditTransactionStatus::InvalidTransaction;
		}
	}
	if (!decode_commit(
			commit->payload,
			output.transaction,
			commands->payload
		)) {
		output = {};
		return WtEditTransactionStatus::HashMismatch;
	}
	return WtEditTransactionStatus::Ok;
}

} // namespace world_transvoxel
