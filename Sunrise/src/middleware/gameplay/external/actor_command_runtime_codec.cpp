#include "actor_command_runtime_codec.h"

#include <array>
#include <limits>

namespace sunrise::middleware::gameplay::external {
namespace {

namespace format = state::activity_sdk::format;
namespace wire = actor_wire;

/** A command selector is seven bits. */
constexpr std::uint64_t kMaximumCommandSelector = 0x7FU;
/** The walker view stores the type code and codec parameter 2 as one byte each. */
constexpr std::uint32_t kMaximumByteField = 0xFFU;
/** Activity-family type codes run 0 through 46, which is all the fallback can check. */
constexpr std::uint8_t kActivityTypeCodeCount = 47;
/** Structural values every type-36 body carries before its payload. */
constexpr std::size_t kCommandHeaderValueCount = 7;
/** The command mode is three bits. */
constexpr std::uint8_t kMaximumCommandMode = 7;
/** The default and target-reference fields are one bit each. */
constexpr std::uint8_t kMaximumCommandBit = 1;
/** Activity types 29 and 43 share the primary-reference and tag/index wire body. */
constexpr std::uint8_t kReferenceValueType = 29;
constexpr std::uint8_t kReferenceValuePairType = 43;

/** Activity aliases must not change the raw reference forms used by other families. */
[[nodiscard]] std::uint8_t sdk_canonical_type(const void*, std::uint8_t type) noexcept {
    return type == kReferenceValueType ? kReferenceValuePairType : type;
}

/** Carries one selected message while delegating ordinary reflection. */
struct ResolverContext final {
    const ActorCommandCatalog* catalog{};
    const format::ActorMessageSchema* message{};
    wire::RuntimeSchemaResolver reflection{};
};

/** Rejects catalog ranges that wrap or escape their owning span. */
[[nodiscard]] bool valid_range(format::Range range, std::size_t size) noexcept {
    return range.first <= size && range.count <= size - range.first;
}

/** Requires every borrowed SDK span used by the runtime codec. */
[[nodiscard]] bool valid_catalog(const ActorCommandCatalog& catalog) noexcept {
    return !catalog.messages.empty() && !catalog.commands.empty() && !catalog.schemas.empty()
           && !catalog.fields.empty();
}

/** Resolves a unique schema handle into the codec's compact view. */
[[nodiscard]] bool
sdk_find_schema(const void* raw, std::uint32_t handle, wire::runtime::SchemaView& output) noexcept {
    const auto* const catalog = static_cast<const ActorCommandCatalog*>(raw);
    if (catalog == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < catalog->schemas.size(); ++index) {
        const format::RuntimeSchema& row = catalog->schemas[index];
        if (row.handle != handle) {
            continue;
        }
        /** Only these two flags are understood; any other flag refuses the row. */
        constexpr std::uint32_t kAllowedFlags =
            format::kRuntimeSchemaExact | format::kRuntimeSchemaArrayRegion;
        const bool arrayRegion = (row.flags & format::kRuntimeSchemaArrayRegion) != 0;
        if ((row.flags & format::kRuntimeSchemaExact) == 0 || (row.flags & ~kAllowedFlags) != 0
            || arrayRegion != (row.arrayElementCount != 0)
            || row.fields.first > catalog->fields.size()
            || row.fields.count > catalog->fields.size() - row.fields.first) {
            return false;
        }
        output = {static_cast<std::uint32_t>(index),
                  row.handle,
                  row.arrayElementCount,
                  row.fields.first,
                  row.fields.count,
                  row.decodedSize};
        return true;
    }
    return false;
}

/** Reads one schema row without exposing SDK storage to the walker. */
[[nodiscard]] bool sdk_read_schema(const void* raw,
                                   std::uint32_t rowIndex,
                                   wire::runtime::SchemaView& output) noexcept {
    const auto* const catalog = static_cast<const ActorCommandCatalog*>(raw);
    return catalog != nullptr && rowIndex < catalog->schemas.size()
           && sdk_find_schema(raw, catalog->schemas[rowIndex].handle, output)
           && output.row == rowIndex;
}

/** Converts SDK field parameters into the generic walker contract. */
[[nodiscard]] bool
sdk_read_field(const void* raw, std::uint32_t rowIndex, wire::runtime::FieldView& output) noexcept {
    const auto* const catalog = static_cast<const ActorCommandCatalog*>(raw);
    if (catalog == nullptr || rowIndex >= catalog->fields.size()) {
        return false;
    }
    const format::RuntimeField& row = catalog->fields[rowIndex];
    if (row.schemaIndex >= catalog->schemas.size() || (row.flags & format::kRuntimeFieldExact) == 0
        || row.typeCode > kMaximumByteField) {
        return false;
    }
    std::uint32_t nestedRow = wire::runtime::kAbsentRuntimeRow;
    if (row.nestedHandle != format::kAbsentIndex) {
        wire::runtime::SchemaView nested{};
        if (!sdk_find_schema(raw, row.nestedHandle, nested)) {
            return false;
        }
        nestedRow = nested.row;
    }
    std::int32_t bias = 0;
    if (row.bias != format::kAbsentSignedValue) {
        if (row.bias < (std::numeric_limits<std::int32_t>::min)()
            || row.bias > (std::numeric_limits<std::int32_t>::max)()) {
            return false;
        }
        bias = static_cast<std::int32_t>(row.bias);
    }
    const bool dynamicArray = (row.flags & format::kRuntimeFieldDynamicArray) != 0;
    const bool quantizedFloat =
        row.typeCode == static_cast<std::uint32_t>(format::RuntimeFieldType::quantizedFloat);
    const std::int32_t bits = quantizedFloat ? static_cast<std::int32_t>(row.codecParameters[1])
                              : dynamicArray ? static_cast<std::int32_t>(row.codecParameters[1])
                              : row.bits == format::kAbsentIndex
                                  ? 0
                                  : static_cast<std::int32_t>(row.bits);
    output = {rowIndex,
              nestedRow,
              row.structOffset,
              quantizedFloat ? static_cast<std::int32_t>(row.codecParameters[0])
              : dynamicArray ? 1
                             : bias,
              bits,
              sdk_canonical_type(raw, static_cast<std::uint8_t>(row.typeCode)),
              static_cast<std::uint8_t>((row.flags & format::kRuntimeFieldPresenceBit) != 0),
              static_cast<std::uint8_t>(
                  row.codecParameters[2] <= kMaximumByteField ? row.codecParameters[2] : 0)};
    return true;
}

/** Resolves one unique command payload from the complete command catalog. */
[[nodiscard]] bool
sdk_resolve_payload(const void* raw, std::uint8_t selector, std::uint32_t& output) noexcept {
    const auto* const catalog = static_cast<const ActorCommandCatalog*>(raw);
    const format::ActorCommandDefinition* match = nullptr;
    if (catalog == nullptr) {
        return false;
    }
    for (const format::ActorCommandDefinition& command : catalog->commands) {
        if (command.selector != selector) {
            continue;
        }
        if (match != nullptr || command.payloadHandle == 0
            || command.flags != format::kActorCommandDefinitionExact) {
            return false;
        }
        match = &command;
    }
    if (match == nullptr) {
        return false;
    }
    output = match->payloadHandle;
    return true;
}

/** Variant selectors must exist in the activity reflection family. */
[[nodiscard]] bool sdk_validate_type(const void* raw, std::uint8_t typeCode) noexcept {
    const auto* const catalog = static_cast<const ActorCommandCatalog*>(raw);
    if (catalog == nullptr || catalog->owner == nullptr) {
        return typeCode < kActivityTypeCodeCount;
    }
    return state::activity_sdk::runtime_type_by_code(
               *catalog->owner, format::RuntimeCodecFamily::activity, typeCode)
           != nullptr;
}

/** Accepts only exact published no-op types whose wire width is zero. */
[[nodiscard]] bool sdk_zero_bit_type(const void* raw, std::uint8_t typeCode) noexcept {
    const auto* const catalog = static_cast<const ActorCommandCatalog*>(raw);
    if (catalog == nullptr || catalog->owner == nullptr) {
        return false;
    }
    const format::RuntimeTypeDefinition* const row = state::activity_sdk::runtime_type_by_code(
        *catalog->owner, format::RuntimeCodecFamily::activity, typeCode);
    return row != nullptr && (row->flags & format::kRuntimeTypeDefinitionExact) != 0
           && (row->flags & format::kRuntimeTypeUnsupported) != 0 && row->fixedBits == 0
           && row->minimumBits == 0 && row->maximumBits == 0 && row->reserved == 0;
}

/** Accepts only exact type-36 message rows with valid command ranges. */
[[nodiscard]] bool valid_message(const ActorCommandCatalog& catalog,
                                 std::uint32_t index,
                                 const format::ActorMessageSchema*& output) noexcept {
    if (!valid_catalog(catalog) || index >= catalog.messages.size()) {
        return false;
    }
    const format::ActorMessageSchema& message = catalog.messages[index];
    if (message.definitionHandle == 0 || message.durableKey == 0 || message.ownerClass == 0
        || message.commands.count == 0 || !valid_range(message.commands, catalog.commands.size())
        || message.flags != format::kActorMessageSchemaExact || message.reserved != 0) {
        return false;
    }
    output = &message;
    return true;
}

/** Accepts only the command row owned by the selected message. */
[[nodiscard]] bool valid_command(const ActorCommandCatalog& catalog,
                                 const format::ActorMessageSchema& message,
                                 std::uint32_t index,
                                 const format::ActorCommandDefinition*& output) noexcept {
    if (index < message.commands.first
        || index >= message.commands.first + message.commands.count) {
        return false;
    }
    const format::ActorCommandDefinition& command = catalog.commands[index];
    if (command.selector > kMaximumCommandSelector || command.payloadHandle == 0
        || command.flags != format::kActorCommandDefinitionExact) {
        return false;
    }
    output = &command;
    return true;
}

/** Delegates schema lookup through the selected-message resolver. */
[[nodiscard]] bool
find_schema(const void* raw, std::uint32_t handle, wire::runtime::SchemaView& output) noexcept {
    const auto* const context = static_cast<const ResolverContext*>(raw);
    return context != nullptr && context->reflection.findSchema != nullptr
           && context->reflection.findSchema(context->reflection.context, handle, output);
}

/** Delegates schema-row reads through the selected-message resolver. */
[[nodiscard]] bool
read_schema(const void* raw, std::uint32_t row, wire::runtime::SchemaView& output) noexcept {
    const auto* const context = static_cast<const ResolverContext*>(raw);
    return context != nullptr && context->reflection.readSchema != nullptr
           && context->reflection.readSchema(context->reflection.context, row, output);
}

/** Delegates field-row reads through the selected-message resolver. */
[[nodiscard]] bool
read_field(const void* raw, std::uint32_t row, wire::runtime::FieldView& output) noexcept {
    const auto* const context = static_cast<const ResolverContext*>(raw);
    return context != nullptr && context->reflection.readField != nullptr
           && context->reflection.readField(context->reflection.context, row, output);
}

/** Restricts command selection to the current message's command range. */
[[nodiscard]] bool
resolve_payload(const void* raw, std::uint8_t selector, std::uint32_t& output) noexcept {
    const auto* const context = static_cast<const ResolverContext*>(raw);
    if (context == nullptr || context->catalog == nullptr || context->message == nullptr) {
        return false;
    }
    const format::Range range = context->message->commands;
    const format::ActorCommandDefinition* match = nullptr;
    for (std::uint32_t offset = 0; offset < range.count; ++offset) {
        const format::ActorCommandDefinition& command =
            context->catalog->commands[range.first + offset];
        if (command.selector != selector) {
            continue;
        }
        if (match != nullptr || command.payloadHandle == 0
            || command.flags != format::kActorCommandDefinitionExact) {
            return false;
        }
        match = &command;
    }
    if (match == nullptr) {
        return false;
    }
    output = match->payloadHandle;
    return true;
}

/** Delegates union-selector validation through the borrowed reflection resolver. */
[[nodiscard]] bool validate_type(const void* raw, std::uint8_t typeCode) noexcept {
    const auto* const context = static_cast<const ResolverContext*>(raw);
    return context != nullptr && context->reflection.validateType != nullptr
           && context->reflection.validateType(context->reflection.context, typeCode);
}

/** Delegates the exact zero-bit no-op classification through borrowed reflection. */
[[nodiscard]] bool zero_bit_type(const void* raw, std::uint8_t typeCode) noexcept {
    const auto* const context = static_cast<const ResolverContext*>(raw);
    return context != nullptr && context->reflection.isZeroBitType != nullptr
           && context->reflection.isZeroBitType(context->reflection.context, typeCode);
}

/** Delegates family-specific aliases through the borrowed reflection context. */
[[nodiscard]] std::uint8_t canonical_type(const void* raw, std::uint8_t typeCode) noexcept {
    const auto& context = *static_cast<const ResolverContext*>(raw);
    return context.reflection.canonicalType != nullptr
               ? context.reflection.canonicalType(context.reflection.context, typeCode)
               : typeCode;
}

/**
 * Overrides command selection while preserving ordinary reflection.
 * @param context Borrowed message and reflection state, which must outlive the adapter.
 * @return An adapter retaining the source codec's type aliases.
 */
[[nodiscard]] wire::RuntimeSchemaResolver resolver(ResolverContext& context) noexcept {
    wire::RuntimeSchemaResolver output{&context,
                                       find_schema,
                                       read_schema,
                                       read_field,
                                       resolve_payload,
                                       validate_type,
                                       zero_bit_type};
    output.canonicalType = canonical_type;
    return output;
}

/** Finds the message schema's single dynamic actor-command field. */
[[nodiscard]] bool root_field(const format::ActorMessageSchema& message,
                              const wire::RuntimeSchemaResolver& reflection,
                              wire::runtime::SchemaView& schema,
                              wire::runtime::FieldView& field) noexcept {
    return reflection.findSchema != nullptr && reflection.readField != nullptr
           && reflection.findSchema(reflection.context, message.definitionHandle, schema)
           && schema.handle == message.definitionHandle && schema.arrayLength == 0
           && schema.fieldCount == 1
           && reflection.readField(reflection.context, schema.firstField, field)
           && field.row == schema.firstField && field.typeCode == message.bodyType;
}

/** Retains every published identity needed to replay the command. */
[[nodiscard]] ActorCommandIdentity
identity(std::uint32_t messageIndex,
         std::uint32_t commandIndex,
         const format::ActorMessageSchema& message,
         const format::ActorCommandDefinition& command) noexcept {
    return {messageIndex,
            commandIndex,
            message.definitionHandle,
            message.durableKey,
            message.ownerClass,
            message.handlerSlot,
            command.selector,
            command.payloadHandle};
}

} // namespace

/**
 * Builds the generic activity-family reflection adapter.
 * @param catalog Borrowed SDK rows, which must outlive the adapter.
 * @return An adapter using the activity family's field and union aliases.
 */
wire::RuntimeSchemaResolver
actor_runtime_schema_resolver(const ActorCommandCatalog& catalog) noexcept {
    wire::RuntimeSchemaResolver output{&catalog,
                                       sdk_find_schema,
                                       sdk_read_schema,
                                       sdk_read_field,
                                       sdk_resolve_payload,
                                       sdk_validate_type,
                                       sdk_zero_bit_type};
    output.canonicalType = sdk_canonical_type;
    return output;
}

/** Encodes one type-36 command and its selector-owned payload. */
bool encode_actor_command_body(const ActorCommandCatalog& catalog,
                               std::uint32_t messageIndex,
                               std::uint32_t commandIndex,
                               const ActorCommandHeader& header,
                               std::span<const wire::RuntimeDraftValue> payloadValues,
                               const wire::RuntimeSchemaResolver& reflection,
                               std::span<std::byte> output,
                               std::size_t& written,
                               std::size_t& writtenBits,
                               wire::CodecStatus& status,
                               ActorCommandIdentity& outputIdentity) noexcept {
    written = 0;
    writtenBits = 0;
    status = wire::CodecStatus::needsRuntimeSchema;
    outputIdentity = {};
    const format::ActorMessageSchema* message = nullptr;
    const format::ActorCommandDefinition* command = nullptr;
    wire::runtime::SchemaView schema{};
    wire::runtime::FieldView field{};
    if (!valid_message(catalog, messageIndex, message)
        || !valid_command(catalog, *message, commandIndex, command)
        || !root_field(*message, reflection, schema, field)
        || payloadValues.size() > wire::kRuntimeValueCapacity - kCommandHeaderValueCount
        || header.defaultValue > kMaximumCommandBit || header.mode > kMaximumCommandMode
        || header.targetReference > kMaximumCommandBit) {
        status = wire::CodecStatus::unsupportedField;
        return false;
    }

    std::array<wire::RuntimeDraftValue, wire::kRuntimeValueCapacity> values{};
    /** Header roles in wire order; the values below are staged in the same order. */
    constexpr std::array<wire::ValueRole, kCommandHeaderValueCount> roles{
        wire::ValueRole::commandDefault,
        wire::ValueRole::commandMode,
        wire::ValueRole::commandSelector,
        wire::ValueRole::commandTargetReference,
        wire::ValueRole::commandTarget,
        wire::ValueRole::commandAuxiliary16,
        wire::ValueRole::commandAuxiliary8,
    };
    const std::array<std::uint64_t, kCommandHeaderValueCount> headerValues{
        header.defaultValue,
        header.mode,
        command->selector,
        header.targetReference,
        header.target,
        header.auxiliary16,
        header.auxiliary8,
    };
    for (std::size_t index = 0; index < roles.size(); ++index) {
        wire::RuntimeDraftValue& value = values[index];
        value.schemaHandle = schema.handle;
        value.schemaRow = schema.row;
        value.fieldRow = field.row;
        value.role = roles[index];
        value.kind = wire::ValueKind::unsignedInteger;
        value.unsignedValue = headerValues[index];
        value.present = true;
    }
    for (std::size_t index = 0; index < payloadValues.size(); ++index) {
        const wire::RuntimeDraftValue& value = payloadValues[index];
        if (value.schemaHandle != command->payloadHandle) {
            status = wire::CodecStatus::unsupportedField;
            return false;
        }
        values[roles.size() + index] = value;
    }

    ResolverContext context{&catalog, message, reflection};
    const wire::RuntimeSchemaResolver selected = resolver(context);
    if (!wire::encode_full_schema(message->definitionHandle,
                                  std::span(values).first(roles.size() + payloadValues.size()),
                                  selected,
                                  output,
                                  written,
                                  writtenBits,
                                  status)) {
        return false;
    }
    outputIdentity = identity(messageIndex, commandIndex, *message, *command);
    return true;
}

/** Decodes one type-36 command into structural and payload values. */
bool decode_actor_command_body(const ActorCommandCatalog& catalog,
                               std::uint32_t messageIndex,
                               std::span<const std::byte> payload,
                               std::size_t bitCount,
                               const wire::RuntimeSchemaResolver& reflection,
                               std::span<wire::RuntimeDecodedValue> values,
                               wire::RuntimeDecodeResult& result,
                               ActorCommandIdentity& outputIdentity) noexcept {
    result = {};
    outputIdentity = {};
    const format::ActorMessageSchema* message = nullptr;
    wire::runtime::SchemaView schema{};
    wire::runtime::FieldView field{};
    if (!valid_message(catalog, messageIndex, message)
        || !root_field(*message, reflection, schema, field)) {
        result.status = wire::CodecStatus::unsupportedField;
        return false;
    }
    ResolverContext context{&catalog, message, reflection};
    const wire::RuntimeSchemaResolver selected = resolver(context);
    if (!wire::decode_full_schema(
            message->definitionHandle, payload, bitCount, selected, values, result)) {
        return false;
    }

    const wire::RuntimeDecodedValue* selector = nullptr;
    for (std::size_t index = 0; index < result.valueCount; ++index) {
        const wire::RuntimeDecodedValue& value = values[index];
        if (value.schemaHandle == schema.handle && value.schemaRow == schema.row
            && value.fieldRow == field.row && value.role == wire::ValueRole::commandSelector) {
            if (selector != nullptr || value.unsignedValue > kMaximumCommandSelector) {
                result.status = wire::CodecStatus::malformed;
                return false;
            }
            selector = &value;
        }
    }
    if (selector == nullptr) {
        result.status = wire::CodecStatus::malformed;
        return false;
    }
    const format::Range range = message->commands;
    const format::ActorCommandDefinition* command = nullptr;
    std::uint32_t commandIndex = format::kAbsentIndex;
    for (std::uint32_t offset = 0; offset < range.count; ++offset) {
        const std::uint32_t index = range.first + offset;
        const format::ActorCommandDefinition& candidate = catalog.commands[index];
        if (candidate.selector != selector->unsignedValue) {
            continue;
        }
        if (command != nullptr || !valid_command(catalog, *message, index, command)) {
            result.status = wire::CodecStatus::malformed;
            return false;
        }
        command = &candidate;
        commandIndex = index;
    }
    if (command == nullptr) {
        result.status = wire::CodecStatus::needsRuntimeSchema;
        return false;
    }
    outputIdentity = identity(messageIndex, commandIndex, *message, *command);
    return true;
}

} // namespace sunrise::middleware::gameplay::external
