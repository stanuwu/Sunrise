#include <bit>

#include "../../../core/logging/log.h"
#include "activity_sdk_actor_rsat_inventory_internal.h"
#include "activity_sdk_decode_plans_internal.h"

namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory {
namespace {

namespace reflection = decode_plans::internal;
using FieldType = format::RuntimeFieldType;

/** Codec families available to records without an activity codec block. */
inline constexpr std::uint32_t kSobjectCodecFamilies =
    static_cast<std::uint32_t>(format::RuntimeCodecFamily::sobjectModeZero)
    | static_cast<std::uint32_t>(format::RuntimeCodecFamily::sobjectModeOne);

/** Projects one native codec descriptor into the published field layout. */
[[nodiscard]] RuntimeField runtime_field(const reflection::Field& input,
                                         std::uint32_t schemaIndex,
                                         std::uint32_t ordinal) noexcept {
    const auto type = static_cast<FieldType>(input.type);
    RuntimeField field{};
    field.schemaIndex = schemaIndex;
    field.ordinal = ordinal;
    field.structOffset = input.offset;
    field.alternateOffset = input.alternate;
    field.typeCode = input.type;
    field.nestedHandle = input.nested;
    field.codecParameters = {std::bit_cast<std::uint32_t>(input.bias),
                             std::bit_cast<std::uint32_t>(input.width),
                             input.parameter3,
                             input.parameter4};
    field.flags = format::kRuntimeFieldExact;
    if (input.presence != 0) {
        field.flags |= format::kRuntimeFieldPresenceBit;
    }
    if (type == FieldType::nested) {
        field.flags |= format::kRuntimeFieldNestedSchema;
        if (input.bias != 0) {
            field.flags |= format::kRuntimeFieldDynamicArray;
            field.bias = input.bias;
            field.bits = std::bit_cast<std::uint32_t>(input.width);
        }
    } else if (type == FieldType::boolean) {
        field.bias = 0;
        field.bits = 1;
    } else if (type >= FieldType::signed8 && type <= FieldType::unsigned64) {
        field.bias = input.bias;
        field.bits = std::bit_cast<std::uint32_t>(input.width);
    } else if (type == FieldType::quantizedFloat) {
        // Quantized floats keep their bias in the width word and their bit count in parameter 3.
        field.bias = input.width;
        field.bits = input.parameter3;
    }
    // Types past the integers need their own codec, except one that carries no value.
    if (type > FieldType::unsigned64 && type != FieldType::noopSeventeen) {
        field.flags |= format::kRuntimeFieldCustomCodec;
    }
    if (type == FieldType::actorCommand) {
        field.flags |= format::kRuntimeFieldDynamicActorCommand;
    }
    return field;
}

/** Builds schema rows in slot order and attaches each field to its owning row. */
[[nodiscard]] bool read_runtime_schemas(const decode_plans::Image& image,
                                        Snapshot& snapshot,
                                        const char*& error) noexcept {
    try {
        snapshot.runtimeSchemas.clear();
        snapshot.runtimeFields.clear();
        snapshot.runtimeSchemas.reserve(image.slots().size());
        for (const auto& [handle, address] : image.slots()) {
            reflection::Metadata record{};
            if (!reflection::parse_native(image, address, record, error)) {
                return false;
            }
            const reflection::Codec& codec = record.codec;
            if (codec.present && codec.handle != handle) {
                error = "codec_handle_mismatch";
                return false;
            }
            if (codec.fields.size() > format::kAbsentIndex - snapshot.runtimeFields.size()) {
                error = "runtime_capacity";
                return false;
            }
            RuntimeSchema schema{};
            schema.handle = handle;
            schema.decodedSize = record.size;
            schema.definitionHash = record.definitionHash;
            schema.definitionClass = record.definitionClass;
            schema.codecFamilies =
                codec.present ? static_cast<std::uint32_t>(format::RuntimeCodecFamily::activity)
                              : kSobjectCodecFamilies;
            schema.fields = {static_cast<std::uint32_t>(snapshot.runtimeFields.size()),
                             static_cast<std::uint32_t>(codec.fields.size())};
            // The record's address in the image that was read, relocated when read live.
            schema.evidenceAddress = address;
            schema.flags = format::kRuntimeSchemaExact;
            schema.arrayElementCount = codec.array;
            if (codec.array != 0) {
                schema.flags |= format::kRuntimeSchemaArrayRegion;
            }
            const auto schemaIndex = static_cast<std::uint32_t>(snapshot.runtimeSchemas.size());
            for (std::uint32_t ordinal = 0; ordinal < schema.fields.count; ++ordinal) {
                snapshot.runtimeFields.push_back(
                    runtime_field(codec.fields[ordinal], schemaIndex, ordinal));
            }
            snapshot.runtimeSchemas.push_back(schema);
        }
        return true;
    } catch (...) {
        error = "allocation";
        return false;
    }
}

} // namespace

/** Reads runtime schemas and codec fields from the selected reflection image. */
bool add_runtime_schemas(const decode_plans::Image& image, Snapshot& snapshot) noexcept {
    const char* error = nullptr;
    if (read_runtime_schemas(image, snapshot, error)) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::info,
                          "ev=actor_rsat stage=runtime_schemas result=ok schemas=%zu fields=%zu",
                          snapshot.runtimeSchemas.size(),
                          snapshot.runtimeFields.size());
        return true;
    }
    core::log::writef(core::log::Channel::client,
                      core::log::Level::error,
                      "ev=actor_rsat stage=runtime_schemas result=failed reason=%s",
                      error);
    return false;
}

} // namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory
