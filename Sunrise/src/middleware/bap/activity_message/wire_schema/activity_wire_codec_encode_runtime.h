#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "../../../encoding/bit_writer.h"
#include "activity_wire_codec_internal.h"

// The selected-schema writer. It walks one resolved SDK schema and asks a source for every
// scalar it must write, so a message-bound encode and a standalone encode share one walk.

namespace sunrise::middleware::bap::activity_message::wire_schema {

/** @return The level index one real quantizes to. Values outside the range clamp. */
[[nodiscard]] inline std::uint64_t encode_runtime_real(float value,
                                                       const runtime::FieldView& field) noexcept {
    if (field.parameter2 == 0 || field.parameter2 >= 32) {
        return std::bit_cast<std::uint32_t>(value);
    }
    const float minimum = std::bit_cast<float>(static_cast<std::uint32_t>(field.biasOrDynamic));
    const float maximum =
        std::bit_cast<float>(static_cast<std::uint32_t>(field.widthOrCountOffset));
    const std::uint64_t levelCount = std::uint64_t{1} << field.parameter2;
    if (value <= minimum) {
        return 0;
    }
    if (value >= maximum) {
        return levelCount - 1;
    }
    const float step = (maximum - minimum) / static_cast<float>(levelCount - 2);
    const float centered = (value - (minimum + step * 0.5F)) / step;
    const auto interior = static_cast<std::int64_t>(centered + 0.5F) + 1;
    return static_cast<std::uint64_t>(
        (std::clamp)(interior, std::int64_t{1}, static_cast<std::int64_t>(levelCount - 2)));
}

/** Scalar payload copied out of either selected draft representation. */
struct RuntimeAuthoredValue final {
    std::uint64_t unsignedValue{};
    std::int64_t signedValue{};
    float realValue{};
    ValueKind kind{ValueKind::unsignedInteger};
    bool present{};
};

/** @return Whether one authored scalar fits the field's declared type and width. */
[[nodiscard]] inline bool valid_runtime_scalar(const runtime::FieldView& field,
                                               const RuntimeAuthoredValue& value) noexcept {
    if (field.typeCode == 2) {
        return value.kind == ValueKind::boolean && value.unsignedValue <= 1;
    }
    if (field.typeCode == 11) {
        return value.kind == ValueKind::real32 && std::isfinite(value.realValue);
    }
    if (field.typeCode == 35) {
        return value.kind == ValueKind::unsignedInteger;
    }
    const std::uint8_t width = storage_width(field.typeCode);
    if (width == 0) {
        return false;
    }
    if (signed_type(field.typeCode)) {
        if (value.kind != ValueKind::signedInteger) {
            return false;
        }
        if (width == 64) {
            return true;
        }
        const std::int64_t limit = std::int64_t{1} << (width - 1);
        return value.signedValue >= -limit && value.signedValue < limit;
    }
    return value.kind == ValueKind::unsignedInteger
           && (width == 64 || value.unsignedValue <= mask(width));
}

/** Complete selected-runtime-schema writer shared by messages and standalone Slot bodies. */
template <typename Source> class RuntimeSchemaEncoder final {
public:
    RuntimeSchemaEncoder(const RuntimeSchemaResolver& resolver,
                         bits::Writer& writer,
                         Source& source) noexcept
        : resolver_(resolver), writer_(writer), source_(source) {}

    /** Encodes one root and requires every supplied value to be consumed. */
    [[nodiscard]] RuntimeWalkStatus full(const runtime::SchemaView& schema) noexcept {
        return walk_schema(schema, 0);
    }

private:
    [[nodiscard]] bool step() noexcept {
        return walk_.step();
    }

    [[nodiscard]] std::uint32_t next_occurrence(std::uint32_t fieldRow) noexcept {
        return walk_.next_occurrence(fieldRow);
    }

    /** Encodes one struct schema in published field order. */
    [[nodiscard]] RuntimeWalkStatus walk_schema(const runtime::SchemaView& schema,
                                                std::size_t depth) noexcept {
        if (!valid_runtime_schema(schema) || resolver_.readField == nullptr) {
            return RuntimeWalkStatus::schemaUnavailable;
        }
        if (depth > kMaximumRuntimeDepth || !step()) {
            return RuntimeWalkStatus::unsafeCount;
        }
        if (schema.arrayLength != 0) {
            return walk_array(schema, schema.arrayLength, depth);
        }
        RuntimeMemoryMap memory{};
        const RuntimeWalkStatus selected = memory.select(resolver_, schema);
        if (selected != RuntimeWalkStatus::complete) {
            return selected;
        }
        for (std::uint32_t ordinal = 0; ordinal < schema.fieldCount; ++ordinal) {
            const std::uint32_t row = schema.firstField + ordinal;
            runtime::FieldView field{};
            if (!resolver_.readField(resolver_.context, row, field) || field.row != row) {
                return RuntimeWalkStatus::schemaUnavailable;
            }
            const RuntimeWalkStatus status = walk_field(schema, field, 1, memory, depth);
            if (status != RuntimeWalkStatus::complete) {
                return status;
            }
        }
        return walk_.overflowed() ? RuntimeWalkStatus::unsafeCount : RuntimeWalkStatus::complete;
    }

    /** Encodes `count` runtime records of one array; the schema's array length bounds it. */
    [[nodiscard]] RuntimeWalkStatus
    walk_array(const runtime::SchemaView& schema, std::uint32_t count, std::size_t depth) noexcept {
        if (!valid_runtime_schema(schema) || schema.arrayLength == 0 || count > schema.arrayLength
            || !step()) {
            return RuntimeWalkStatus::unsafeCount;
        }
        if (count == 0) {
            return RuntimeWalkStatus::complete;
        }
        runtime::FieldView field{};
        if (resolver_.readField == nullptr
            || !resolver_.readField(resolver_.context, schema.firstField, field)
            || field.row != schema.firstField) {
            return RuntimeWalkStatus::schemaUnavailable;
        }
        RuntimeMemoryMap memory{};
        return walk_field(schema, field, count, memory, depth);
    }

    /** Applies fixed or count-backed repetition to a nested schema. */
    [[nodiscard]] RuntimeWalkStatus walk_nested(const runtime::SchemaView& owner,
                                                const runtime::FieldView& field,
                                                RuntimeMemoryMap& memory,
                                                std::size_t depth) noexcept {
        runtime::SchemaView nested{};
        if (field.nestedSchemaRow == runtime::kAbsentRuntimeRow || resolver_.readSchema == nullptr
            || !resolver_.readSchema(resolver_.context, field.nestedSchemaRow, nested)
            || nested.row != field.nestedSchemaRow || !valid_runtime_schema(nested)) {
            return RuntimeWalkStatus::schemaUnavailable;
        }
        if (field.biasOrDynamic == 1) {
            std::uint32_t capacity = 0;
            if (field.widthOrCountOffset < 0 || !dynamic_capacity(owner, field, nested, capacity)) {
                return RuntimeWalkStatus::unsafeCount;
            }
            std::int64_t dynamicCount = 0;
            if (!memory.find(static_cast<std::uint32_t>(field.widthOrCountOffset), dynamicCount)
                || dynamicCount < 0 || static_cast<std::uint64_t>(dynamicCount) > capacity) {
                return RuntimeWalkStatus::unsafeCount;
            }
            if (nested.arrayLength != 0) {
                return walk_array(nested, static_cast<std::uint32_t>(dynamicCount), depth + 1);
            }
            for (std::int64_t index = 0; index < dynamicCount; ++index) {
                const RuntimeWalkStatus status = walk_schema(nested, depth + 1);
                if (status != RuntimeWalkStatus::complete) {
                    return status;
                }
            }
            return RuntimeWalkStatus::complete;
        }
        if (field.biasOrDynamic != 0) {
            return RuntimeWalkStatus::schemaUnavailable;
        }
        return nested.arrayLength != 0 ? walk_array(nested, nested.arrayLength, depth + 1)
                                       : walk_schema(nested, depth + 1);
    }

    /** Encodes a nullable handle followed by its selected schema body. */
    [[nodiscard]] RuntimeWalkStatus write_selected(const runtime::SchemaView& owner,
                                                   const runtime::FieldView& field,
                                                   std::uint32_t occurrence,
                                                   std::size_t depth) noexcept {
        RuntimeAuthoredValue value{};
        if (!source_.take(owner, field, occurrence, ValueRole::selectedHandle, value)) {
            return RuntimeWalkStatus::missingValue;
        }
        if (value.kind != ValueKind::unsignedInteger
            || value.unsignedValue > (std::numeric_limits<std::uint32_t>::max)()) {
            return RuntimeWalkStatus::unsupportedField;
        }
        if (!writer_.write(value.present ? 1U : 0U, 1)) {
            return RuntimeWalkStatus::outputTooSmall;
        }
        if (!value.present) {
            return RuntimeWalkStatus::complete;
        }
        const auto handle = static_cast<std::uint32_t>(value.unsignedValue);
        if (!writer_.write(handle, 32)) {
            return RuntimeWalkStatus::outputTooSmall;
        }
        if (handle == runtime::kAbsentRuntimeRow) {
            return RuntimeWalkStatus::complete;
        }
        runtime::SchemaView selected{};
        if (resolver_.findSchema == nullptr
            || !resolver_.findSchema(resolver_.context, handle, selected)
            || selected.handle != handle || !valid_runtime_schema(selected)) {
            return RuntimeWalkStatus::schemaUnavailable;
        }
        return selected.arrayLength != 0 ? walk_array(selected, selected.arrayLength, depth + 1)
                                         : walk_schema(selected, depth + 1);
    }

    /** Applies the published bias and width before writing one scalar. */
    [[nodiscard]] RuntimeWalkStatus write_scalar(const runtime::FieldView& field,
                                                 const RuntimeAuthoredValue& value,
                                                 RuntimeMemoryMap& memory) noexcept {
        if (!valid_runtime_scalar(field, value)) {
            return RuntimeWalkStatus::unsupportedField;
        }
        const bool raw64 = field.typeCode == 35;
        const std::uint8_t nativeWidth = raw64 ? 64 : storage_width(field.typeCode);
        const std::uint8_t declaredWidth = raw64 ? 64 : runtime_declared_width(field);
        if (nativeWidth == 0 || declaredWidth == 0) {
            return RuntimeWalkStatus::unsupportedField;
        }
        std::uint64_t decoded = 0;
        if (raw64) {
            decoded = value.unsignedValue;
        } else if (field.typeCode == 11) {
            decoded = encode_runtime_real(value.realValue, field);
        } else if (signed_type(field.typeCode)) {
            decoded = static_cast<std::uint64_t>(value.signedValue) & mask(nativeWidth);
        } else if (field.typeCode == 2) {
            decoded = value.unsignedValue != 0 ? 1 : 0;
        } else {
            decoded = value.unsignedValue & mask(nativeWidth);
        }
        const std::uint64_t raw =
            raw64 || field.typeCode == 11
                ? decoded
                : (decoded
                   + static_cast<std::uint64_t>(static_cast<std::int64_t>(field.biasOrDynamic)))
                      & mask(nativeWidth);
        if ((raw & ~mask(declaredWidth)) != 0) {
            return RuntimeWalkStatus::unsupportedField;
        }
        if (!writer_.write(raw, declaredWidth)) {
            return RuntimeWalkStatus::outputTooSmall;
        }
        if (!raw64) {
            const std::int64_t memoryValue = signed_type(field.typeCode)
                                                 ? sign_extend(decoded, nativeWidth)
                                                 : static_cast<std::int64_t>(decoded);
            memory.store(field.structOffset, memoryValue);
        }
        return RuntimeWalkStatus::complete;
    }

    /** Writes one type-36 command from explicit structural roles and a resolved payload schema. */
    [[nodiscard]] RuntimeWalkStatus write_command(const runtime::SchemaView& owner,
                                                  const runtime::FieldView& field,
                                                  std::uint32_t occurrence,
                                                  std::size_t depth) noexcept {
        // Type-36 command header fields, in wire order with their bit widths.
        constexpr std::array<std::pair<ValueRole, std::uint8_t>, 7> fields{{
            {ValueRole::commandDefault, std::uint8_t{1}},
            {ValueRole::commandMode, std::uint8_t{3}},
            {ValueRole::commandSelector, std::uint8_t{7}},
            {ValueRole::commandTargetReference, std::uint8_t{1}},
            {ValueRole::commandTarget, std::uint8_t{16}},
            {ValueRole::commandAuxiliary16, std::uint8_t{16}},
            {ValueRole::commandAuxiliary8, std::uint8_t{8}},
        }};
        std::uint8_t selector = 0;
        for (const auto& [role, width] : fields) {
            RuntimeAuthoredValue value{};
            if (!source_.take(owner, field, occurrence, role, value)
                || value.kind != ValueKind::unsignedInteger
                || (value.unsignedValue & ~mask(width)) != 0) {
                return RuntimeWalkStatus::missingValue;
            }
            if (!writer_.write(value.unsignedValue, width)) {
                return RuntimeWalkStatus::outputTooSmall;
            }
            if (resolver_.nativeCommandEmptyShortcut && role == ValueRole::commandDefault
                && value.unsignedValue != 0) {
                return RuntimeWalkStatus::complete;
            }
            if (resolver_.nativeCommandEmptyShortcut && role == ValueRole::commandTargetReference
                && value.unsignedValue != 0) {
                return RuntimeWalkStatus::unsupportedField;
            }
            if (role == ValueRole::commandSelector) {
                selector = static_cast<std::uint8_t>(value.unsignedValue);
            }
        }
        std::uint32_t payloadHandle = 0;
        runtime::SchemaView payload{};
        if (resolver_.resolveCommandPayload == nullptr
            || !resolver_.resolveCommandPayload(resolver_.context, selector, payloadHandle)
            || payloadHandle == 0 || payloadHandle == runtime::kAbsentRuntimeRow
            || resolver_.findSchema == nullptr
            || !resolver_.findSchema(resolver_.context, payloadHandle, payload)
            || payload.handle != payloadHandle || !valid_runtime_schema(payload)) {
            return RuntimeWalkStatus::schemaUnavailable;
        }
        return payload.arrayLength != 0 ? walk_array(payload, payload.arrayLength, depth + 1)
                                        : walk_schema(payload, depth + 1);
    }

    /** Writes one type-18 sentinel or live simulation entity token. */
    [[nodiscard]] RuntimeWalkStatus write_entity_reference(const runtime::SchemaView& owner,
                                                           const runtime::FieldView& field,
                                                           std::uint32_t occurrence) noexcept {
        RuntimeAuthoredValue kind{};
        if (!source_.take(owner, field, occurrence, ValueRole::entityReferenceKind, kind)
            || kind.kind != ValueKind::unsignedInteger || kind.unsignedValue > 2U) {
            return RuntimeWalkStatus::missingValue;
        }
        if (kind.unsignedValue < 2U) {
            return writer_.write(0, 1) && writer_.write(kind.unsignedValue, 1)
                       ? RuntimeWalkStatus::complete
                       : RuntimeWalkStatus::outputTooSmall;
        }
        RuntimeAuthoredValue slot{};
        RuntimeAuthoredValue incarnation{};
        if (!source_.take(owner, field, occurrence, ValueRole::entityReferenceSlot, slot)
            || !source_.take(
                owner, field, occurrence, ValueRole::entityReferenceIncarnation, incarnation)
            || slot.kind != ValueKind::unsignedInteger
            || incarnation.kind != ValueKind::unsignedInteger || slot.unsignedValue > 0x1FFFU
            || incarnation.unsignedValue > 0x0FU) {
            return RuntimeWalkStatus::missingValue;
        }
        return writer_.write(1, 1) && writer_.write(slot.unsignedValue, 13)
                       && writer_.write(incarnation.unsignedValue, 4)
                   ? RuntimeWalkStatus::complete
                   : RuntimeWalkStatus::outputTooSmall;
    }

    /** Writes one type-23 nullable runtime schema handle. */
    [[nodiscard]] RuntimeWalkStatus write_schema_reference(const runtime::SchemaView& owner,
                                                           const runtime::FieldView& field,
                                                           std::uint32_t occurrence) noexcept {
        RuntimeAuthoredValue value{};
        if (!source_.take(owner, field, occurrence, ValueRole::schemaReference, value)
            || value.kind != ValueKind::unsignedInteger
            || value.unsignedValue > (std::numeric_limits<std::uint32_t>::max)()) {
            return RuntimeWalkStatus::missingValue;
        }
        if (!writer_.write(value.present ? 1U : 0U, 1)) {
            return RuntimeWalkStatus::outputTooSmall;
        }
        return !value.present || writer_.write(value.unsignedValue, 32)
                   ? RuntimeWalkStatus::complete
                   : RuntimeWalkStatus::outputTooSmall;
    }

    /** Types 13 and 39 share quantized codes and raw float components. */
    [[nodiscard]] RuntimeWalkStatus write_vector3(const runtime::SchemaView& owner,
                                                  const runtime::FieldView& field,
                                                  std::uint32_t occurrence) noexcept {
        const auto* profile = resolver_.positionProfile;
        bool compressed = false;
        if (profile != nullptr && profile->selectorPresent) {
            RuntimeAuthoredValue value{};
            if (!source_.take(owner, field, occurrence, ValueRole::positionCompressed, value)
                || value.kind != ValueKind::unsignedInteger || value.unsignedValue > 1
                || !writer_.write(value.unsignedValue, 1)) {
                return RuntimeWalkStatus::missingValue;
            }
            compressed = value.unsignedValue != 0;
        }
        if (compressed) {
            if (!profile->hasWidths) {
                return RuntimeWalkStatus::unsupportedField;
            }
            // A compressed position sends X, Y then Z, each at its profile axis width.
            constexpr std::array<ValueRole, 3> roles{
                ValueRole::positionCodeX, ValueRole::positionCodeY, ValueRole::positionCodeZ};
            for (std::size_t index = 0; index < roles.size(); ++index) {
                const auto width = profile->axisBits[index];
                if (width >= 32) {
                    return RuntimeWalkStatus::unsupportedField;
                }
                RuntimeAuthoredValue value{};
                if (!source_.take(owner, field, occurrence, roles[index], value)
                    || value.kind != ValueKind::unsignedInteger
                    || value.unsignedValue >= (std::uint64_t{1} << width)
                    || !writer_.write(value.unsignedValue, width)) {
                    return RuntimeWalkStatus::missingValue;
                }
            }
            return RuntimeWalkStatus::complete;
        }
        for (const ValueRole role : {ValueRole::vectorX, ValueRole::vectorY, ValueRole::vectorZ}) {
            RuntimeAuthoredValue value{};
            if (!source_.take(owner, field, occurrence, role, value)
                || value.kind != ValueKind::real32
                || !writer_.write(std::bit_cast<std::uint32_t>(value.realValue), 32)) {
                return RuntimeWalkStatus::missingValue;
            }
        }
        return RuntimeWalkStatus::complete;
    }

    /** Type 15 preserves the native point, direction, and explicit-W selectors. */
    [[nodiscard]] RuntimeWalkStatus write_vector4(const runtime::SchemaView& owner,
                                                  const runtime::FieldView& field,
                                                  std::uint32_t occurrence) noexcept {
        RuntimeAuthoredValue point{}, direction{};
        if (!source_.take(owner, field, occurrence, ValueRole::positionPoint, point)
            || point.kind != ValueKind::unsignedInteger || point.unsignedValue > 1
            || !writer_.write(point.unsignedValue, 1)) {
            return RuntimeWalkStatus::missingValue;
        }
        if (point.unsignedValue == 0) {
            if (!source_.take(owner, field, occurrence, ValueRole::positionDirection, direction)
                || direction.kind != ValueKind::unsignedInteger || direction.unsignedValue > 1
                || !writer_.write(direction.unsignedValue, 1)) {
                return RuntimeWalkStatus::missingValue;
            }
        }
        if (direction.unsignedValue != 0) {
            for (const auto role : {ValueRole::vectorX, ValueRole::vectorY, ValueRole::vectorZ}) {
                RuntimeAuthoredValue value{};
                if (!source_.take(owner, field, occurrence, role, value)
                    || value.kind != ValueKind::real32
                    || !writer_.write(std::bit_cast<std::uint32_t>(value.realValue), 32)) {
                    return RuntimeWalkStatus::missingValue;
                }
            }
        } else {
            const auto status = write_vector3(owner, field, occurrence);
            if (status != RuntimeWalkStatus::complete) {
                return status;
            }
        }
        if (point.unsignedValue == 0 && direction.unsignedValue == 0) {
            RuntimeAuthoredValue value{};
            if (!source_.take(owner, field, occurrence, ValueRole::vectorW, value)
                || value.kind != ValueKind::real32
                || !writer_.write(std::bit_cast<std::uint32_t>(value.realValue), 32)) {
                return RuntimeWalkStatus::missingValue;
            }
        }
        return RuntimeWalkStatus::complete;
    }

    /** Type 14 retains the zero shortcut or its direction and magnitude codes. */
    [[nodiscard]] RuntimeWalkStatus write_compressed_vector(const runtime::SchemaView& owner,
                                                            const runtime::FieldView& field,
                                                            std::uint32_t occurrence) noexcept {
        RuntimeAuthoredValue zero{};
        if (!source_.take(owner, field, occurrence, ValueRole::compressedVectorZero, zero)
            || zero.kind != ValueKind::boolean
            || !writer_.write(zero.unsignedValue != 0 ? 1U : 0U, 1)) {
            return RuntimeWalkStatus::missingValue;
        }
        if (zero.unsignedValue != 0) {
            return RuntimeWalkStatus::complete;
        }
        RuntimeAuthoredValue direction{};
        RuntimeAuthoredValue magnitude{};
        if (!source_.take(owner, field, occurrence, ValueRole::compressedVectorDirection, direction)
            || !source_.take(
                owner, field, occurrence, ValueRole::compressedVectorMagnitude, magnitude)
            || direction.kind != ValueKind::unsignedInteger
            || magnitude.kind != ValueKind::unsignedInteger || direction.unsignedValue >= (1U << 19)
            || magnitude.unsignedValue >= (1U << 16)) {
            return RuntimeWalkStatus::missingValue;
        }
        return writer_.write(direction.unsignedValue, 19)
                       && writer_.write(magnitude.unsignedValue, 16)
                   ? RuntimeWalkStatus::complete
                   : RuntimeWalkStatus::outputTooSmall;
    }

    /** Writes the exact type-26 nullable record without resolving its semantic relation. */
    [[nodiscard]] RuntimeWalkStatus write_nullable160(const runtime::SchemaView& owner,
                                                      const runtime::FieldView& field,
                                                      std::uint32_t occurrence) noexcept {
        RuntimeAuthoredValue present{};
        if (!source_.take(owner, field, occurrence, ValueRole::nullable160Present, present)
            || present.kind != ValueKind::boolean || present.unsignedValue > 1) {
            return RuntimeWalkStatus::missingValue;
        }
        if (!writer_.write(present.unsignedValue, 1)) {
            return RuntimeWalkStatus::outputTooSmall;
        }
        if (present.unsignedValue == 0) {
            return RuntimeWalkStatus::complete;
        }
        RuntimeAuthoredValue a{};
        RuntimeAuthoredValue b{};
        RuntimeAuthoredValue compact{};
        RuntimeAuthoredValue c{};
        if (!source_.take(owner, field, occurrence, ValueRole::nullable160A, a)
            || !source_.take(owner, field, occurrence, ValueRole::nullable160B, b)
            || !source_.take(owner, field, occurrence, ValueRole::nullable160Compact, compact)
            || !source_.take(owner, field, occurrence, ValueRole::nullable160RawC, c)
            || a.kind != ValueKind::unsignedInteger || b.kind != ValueKind::unsignedInteger
            || compact.kind != ValueKind::boolean || c.kind != ValueKind::unsignedInteger
            || a.unsignedValue > 0xFFFFFFFFULL || b.unsignedValue > 0xFFFFFFFFULL
            || compact.unsignedValue > 1
            || c.unsignedValue >= (std::uint64_t{1} << (compact.unsignedValue != 0 ? 12 : 32))) {
            return RuntimeWalkStatus::unsupportedField;
        }
        const std::uint8_t cWidth = compact.unsignedValue != 0 ? 12 : 32;
        return writer_.write(a.unsignedValue, 32) && writer_.write(b.unsignedValue, 32)
                       && writer_.write(compact.unsignedValue, 1)
                       && writer_.write(c.unsignedValue, cWidth)
                   ? RuntimeWalkStatus::complete
                   : RuntimeWalkStatus::outputTooSmall;
    }

    /** Writes the exact absent sentinel or live entity/depth token used by type 19. */
    [[nodiscard]] RuntimeWalkStatus write_primary_reference(const runtime::SchemaView& owner,
                                                            const runtime::FieldView& field,
                                                            std::uint32_t occurrence) noexcept {
        RuntimeAuthoredValue primaryPresent{};
        if (!source_.take(owner,
                          field,
                          occurrence,
                          ValueRole::referenceValuePrimaryPresent,
                          primaryPresent)) {
            return RuntimeWalkStatus::missingValue;
        }
        if (primaryPresent.kind != ValueKind::boolean || primaryPresent.unsignedValue > 1) {
            return RuntimeWalkStatus::unsupportedField;
        }
        if (!writer_.write(primaryPresent.unsignedValue, 1)) {
            return RuntimeWalkStatus::outputTooSmall;
        }
        if (primaryPresent.unsignedValue == 0) {
            RuntimeAuthoredValue sentinel{};
            if (!source_.take(
                    owner, field, occurrence, ValueRole::referenceValuePrimarySentinel, sentinel)) {
                return RuntimeWalkStatus::missingValue;
            }
            if (sentinel.kind != ValueKind::boolean || sentinel.unsignedValue > 1) {
                return RuntimeWalkStatus::unsupportedField;
            }
            if (!writer_.write(sentinel.unsignedValue, 1)) {
                return RuntimeWalkStatus::outputTooSmall;
            }
        } else {
            RuntimeAuthoredValue slot{};
            RuntimeAuthoredValue incarnation{};
            RuntimeAuthoredValue depth{};
            if (!source_.take(owner, field, occurrence, ValueRole::referenceValuePrimarySlot, slot)
                || !source_.take(owner,
                                 field,
                                 occurrence,
                                 ValueRole::referenceValuePrimaryIncarnation,
                                 incarnation)
                || !source_.take(
                    owner, field, occurrence, ValueRole::referenceValuePrimaryDepth, depth)) {
                return RuntimeWalkStatus::missingValue;
            }
            if (slot.kind != ValueKind::unsignedInteger
                || incarnation.kind != ValueKind::unsignedInteger
                || depth.kind != ValueKind::unsignedInteger || slot.unsignedValue >= (1U << 13)
                || incarnation.unsignedValue >= (1U << 4) || depth.unsignedValue >= (1U << 6)) {
                return RuntimeWalkStatus::unsupportedField;
            }
            if (!writer_.write(slot.unsignedValue, 13)
                || !writer_.write(incarnation.unsignedValue, 4)
                || !writer_.write(depth.unsignedValue, 6)) {
                return RuntimeWalkStatus::outputTooSmall;
            }
        }
        return RuntimeWalkStatus::complete;
    }

    /** Writes both raw references and the selected index width of type 43. */
    [[nodiscard]] RuntimeWalkStatus
    write_reference_value_variant(const runtime::SchemaView& owner,
                                  const runtime::FieldView& field,
                                  std::uint32_t occurrence) noexcept {
        const RuntimeWalkStatus primary = write_primary_reference(owner, field, occurrence);
        if (primary != RuntimeWalkStatus::complete) {
            return primary;
        }
        return write_reference_value(owner, field, occurrence);
    }

    /** Types 27 and 43 share a nullable pair of tags followed by a sized index. */
    [[nodiscard]] RuntimeWalkStatus write_reference_value(const runtime::SchemaView& owner,
                                                          const runtime::FieldView& field,
                                                          std::uint32_t occurrence) noexcept {
        RuntimeAuthoredValue valuePresent{};
        if (!source_.take(
                owner, field, occurrence, ValueRole::referenceValuePresent, valuePresent)) {
            return RuntimeWalkStatus::missingValue;
        }
        if (valuePresent.kind != ValueKind::boolean || valuePresent.unsignedValue > 1) {
            return RuntimeWalkStatus::unsupportedField;
        }
        if (!writer_.write(valuePresent.unsignedValue, 1)) {
            return RuntimeWalkStatus::outputTooSmall;
        }
        if (valuePresent.unsignedValue == 0) {
            return RuntimeWalkStatus::complete;
        }
        RuntimeAuthoredValue tagA{};
        RuntimeAuthoredValue tagB{};
        RuntimeAuthoredValue compact{};
        RuntimeAuthoredValue index{};
        if (!source_.take(owner, field, occurrence, ValueRole::referenceValueTagA, tagA)
            || !source_.take(owner, field, occurrence, ValueRole::referenceValueTagB, tagB)
            || !source_.take(owner, field, occurrence, ValueRole::referenceValueCompact, compact)
            || !source_.take(owner, field, occurrence, ValueRole::referenceValueIndex, index)) {
            return RuntimeWalkStatus::missingValue;
        }
        if (tagA.kind != ValueKind::unsignedInteger || tagB.kind != ValueKind::unsignedInteger
            || compact.kind != ValueKind::boolean || index.kind != ValueKind::unsignedInteger
            || tagA.unsignedValue > 0xFFFFFFFFULL || tagB.unsignedValue > 0xFFFFFFFFULL
            || compact.unsignedValue > 1
            || (compact.unsignedValue != 0 ? index.unsignedValue >= (1U << 12)
                                           : index.unsignedValue > 0xFFFFFFFFULL)) {
            return RuntimeWalkStatus::unsupportedField;
        }
        const std::uint8_t indexWidth = compact.unsignedValue != 0 ? 12 : 32;
        return writer_.write(tagA.unsignedValue, 32) && writer_.write(tagB.unsignedValue, 32)
                       && writer_.write(compact.unsignedValue, 1)
                       && writer_.write(index.unsignedValue, indexWidth)
                   ? RuntimeWalkStatus::complete
                   : RuntimeWalkStatus::outputTooSmall;
    }

    /** Universal variant arms recurse through the validated runtime type table. */
    [[nodiscard]] RuntimeWalkStatus write_inline_type(const runtime::SchemaView& owner,
                                                      runtime::FieldView field,
                                                      std::uint32_t occurrence,
                                                      RuntimeMemoryMap& memory,
                                                      std::size_t depth) noexcept {
        if (depth >= kMaximumRuntimeDepth || !step()) {
            return RuntimeWalkStatus::unsafeCount;
        }
        if (resolver_.canonicalType != nullptr) {
            field.typeCode = resolver_.canonicalType(resolver_.context, field.typeCode);
        }
        if (field.typeCode == 13 || field.typeCode == 39) {
            return write_vector3(owner, field, occurrence);
        }
        if (field.typeCode == 16) {
            RuntimeAuthoredValue shortcut{}, axis{}, angle{};
            if (!source_.take(owner, field, occurrence, ValueRole::rotationAxisShortcut, shortcut)
                || !source_.take(owner, field, occurrence, ValueRole::rotationAxisCode, axis)
                || !source_.take(owner, field, occurrence, ValueRole::rotationAngleCode, angle)
                || shortcut.kind != ValueKind::unsignedInteger || shortcut.unsignedValue > 1
                || axis.kind != ValueKind::unsignedInteger
                || angle.kind != ValueKind::unsignedInteger || angle.unsignedValue > 127) {
                return RuntimeWalkStatus::missingValue;
            }
            const std::uint8_t axisWidth = shortcut.unsignedValue != 0 ? 1 : 19;
            if (axis.unsignedValue > mask(axisWidth)) {
                return RuntimeWalkStatus::missingValue;
            }
            return writer_.write(shortcut.unsignedValue, 1)
                           && writer_.write(axis.unsignedValue, axisWidth)
                           && writer_.write(angle.unsignedValue, 7)
                       ? RuntimeWalkStatus::complete
                       : RuntimeWalkStatus::outputTooSmall;
        }
        if (field.typeCode == 42) {
            RuntimeAuthoredValue uniform{};
            if (!source_.take(owner, field, occurrence, ValueRole::vectorUniform, uniform)
                || uniform.kind != ValueKind::boolean || uniform.unsignedValue > 1
                || !writer_.write(uniform.unsignedValue, 1)) {
                return RuntimeWalkStatus::missingValue;
            }
            const bool quantized = field.parameter2 > 0 && field.parameter2 < 32;
            unsigned width = quantized ? field.parameter2 : 32;
            if (uniform.unsignedValue != 0 && quantized) {
                RuntimeAuthoredValue integer{};
                if (!source_.take(
                        owner, field, occurrence, ValueRole::vectorUniformInteger, integer)
                    || integer.kind != ValueKind::boolean || integer.unsignedValue > 1
                    || !writer_.write(integer.unsignedValue, 1)) {
                    return RuntimeWalkStatus::missingValue;
                }
                if (integer.unsignedValue != 0) {
                    const float minimum =
                        std::bit_cast<float>(static_cast<std::uint32_t>(field.biasOrDynamic));
                    const float maximum =
                        std::bit_cast<float>(static_cast<std::uint32_t>(field.widthOrCountOffset));
                    const float range = maximum - minimum;
                    if (!std::isfinite(range) || range < 0 || range >= 2147483648.0F) {
                        return RuntimeWalkStatus::unsupportedField;
                    }
                    width = std::bit_width(static_cast<std::uint32_t>(range));
                }
            }
            // A compressed vector sends X, Y, Z then W in that order.
            constexpr std::array roles{ValueRole::vectorCodeX,
                                       ValueRole::vectorCodeY,
                                       ValueRole::vectorCodeZ,
                                       ValueRole::vectorCodeW};
            for (unsigned index = 0; index < (uniform.unsignedValue != 0 ? 1U : 4U); ++index) {
                RuntimeAuthoredValue code{};
                if (!source_.take(owner, field, occurrence, roles[index], code)
                    || code.kind != ValueKind::unsignedInteger
                    || code.unsignedValue > mask(static_cast<std::uint8_t>(width))) {
                    return RuntimeWalkStatus::missingValue;
                }
                if (width != 0
                    && !writer_.write(code.unsignedValue, static_cast<std::uint8_t>(width))) {
                    return RuntimeWalkStatus::outputTooSmall;
                }
            }
            return RuntimeWalkStatus::complete;
        }
        if (field.typeCode == 44 || field.typeCode == 45) {
            const auto width =
                field.typeCode == 45
                    ? (field.parameter2 > 0 && field.parameter2 < 32 ? field.parameter2 : 32)
                    : field.widthOrCountOffset;
            RuntimeAuthoredValue code{};
            if (width <= 0 || width > 32
                || !source_.take(owner, field, occurrence, ValueRole::opaqueScalarCode, code)
                || code.kind != ValueKind::unsignedInteger
                || code.unsignedValue > mask(static_cast<std::uint8_t>(width))) {
                return RuntimeWalkStatus::missingValue;
            }
            return writer_.write(code.unsignedValue, static_cast<std::uint8_t>(width))
                       ? RuntimeWalkStatus::complete
                       : RuntimeWalkStatus::outputTooSmall;
        }
        if (field.typeCode == 14 || field.typeCode == 40) {
            return write_compressed_vector(owner, field, occurrence);
        }
        if (field.typeCode == 15) {
            return write_vector4(owner, field, occurrence);
        }
        if (field.typeCode == 18) {
            return write_entity_reference(owner, field, occurrence);
        }
        if (field.typeCode == 19) {
            return write_primary_reference(owner, field, occurrence);
        }
        if (field.typeCode == 22 || field.typeCode == 23) {
            return write_schema_reference(owner, field, occurrence);
        }
        if (field.typeCode == 26) {
            return write_nullable160(owner, field, occurrence);
        }
        if (field.typeCode == 27) {
            return write_reference_value(owner, field, occurrence);
        }
        if (field.typeCode == 43) {
            return write_reference_value_variant(owner, field, occurrence);
        }
        if (field.typeCode == 24 || field.typeCode == 25) {
            RuntimeAuthoredValue selector{};
            if (!source_.take(owner, field, occurrence, ValueRole::variantSelector, selector)
                || selector.kind != ValueKind::unsignedInteger || selector.unsignedValue >= 64
                || resolver_.validateType == nullptr
                || !resolver_.validateType(resolver_.context,
                                           static_cast<std::uint8_t>(selector.unsignedValue))
                || !writer_.write(selector.unsignedValue, 6)) {
                return RuntimeWalkStatus::unsupportedField;
            }
            field.typeCode = static_cast<std::uint8_t>(selector.unsignedValue);
            if (resolver_.isZeroBitType != nullptr
                && resolver_.isZeroBitType(resolver_.context, field.typeCode)) {
                return RuntimeWalkStatus::complete;
            }
            return write_inline_type(owner, field, occurrence, memory, depth + 1);
        }
        if (field.typeCode == 28) {
            runtime::FieldView variant = field;
            variant.typeCode = 25;
            const RuntimeWalkStatus selected =
                write_inline_type(owner, variant, occurrence, memory, depth + 1);
            if (selected != RuntimeWalkStatus::complete) {
                return selected;
            }
            RuntimeAuthoredValue shortForm{};
            RuntimeAuthoredValue index{};
            if (!source_.take(owner, field, occurrence, ValueRole::customIndexShort, shortForm)
                || !source_.take(owner, field, occurrence, ValueRole::customIndex, index)
                || shortForm.kind != ValueKind::boolean || index.kind != ValueKind::unsignedInteger
                || (shortForm.unsignedValue != 0 ? index.unsignedValue >= 4096
                                                 : index.unsignedValue > 0xFFFFFFFFULL)
                || !writer_.write(shortForm.unsignedValue != 0 ? 1U : 0U, 1)
                || !writer_.write(index.unsignedValue, shortForm.unsignedValue != 0 ? 12 : 32)) {
                return RuntimeWalkStatus::unsupportedField;
            }
            return RuntimeWalkStatus::complete;
        }
        RuntimeAuthoredValue value{};
        if (!source_.take(owner, field, occurrence, ValueRole::scalar, value)) {
            return RuntimeWalkStatus::missingValue;
        }
        return write_scalar(field, value, memory);
    }

    /** Encodes every occurrence of one reflected field transactionally. */
    [[nodiscard]] RuntimeWalkStatus walk_field(const runtime::SchemaView& owner,
                                               const runtime::FieldView& field,
                                               std::uint32_t repeats,
                                               RuntimeMemoryMap& memory,
                                               std::size_t depth) noexcept {
        if (!step() || field.row < owner.firstField
            || field.row >= owner.firstField + owner.fieldCount || field.presence > 1) {
            return RuntimeWalkStatus::schemaUnavailable;
        }
        const bool nested = field.typeCode == 1;
        const bool selected = field.typeCode == 34 || field.typeCode == 41;
        const bool command = field.typeCode == 36;
        const bool entityReference = field.typeCode == 18;
        const bool schemaReference = field.typeCode == 23;
        const bool custom = field.typeCode == 13 || field.typeCode == 14 || field.typeCode == 15
                            || field.typeCode == 16 || field.typeCode == 22 || field.typeCode == 19
                            || field.typeCode == 24 || field.typeCode == 25 || field.typeCode == 26
                            || field.typeCode == 27 || field.typeCode == 28 || field.typeCode == 39
                            || field.typeCode == 40 || field.typeCode == 42 || field.typeCode == 43
                            || field.typeCode == 44 || field.typeCode == 45;
        const bool zeroBit = resolver_.isZeroBitType != nullptr
                             && resolver_.isZeroBitType(resolver_.context, field.typeCode);
        // A selected field carries its schema handle on the wire, so it is structural but must
        // not also declare a nested schema row.
        const bool nestedSchemaKind =
            nested || command || entityReference || schemaReference || custom;
        const bool unknownOptional = field.presence != 0 && !nestedSchemaKind && !selected
                                     && !zeroBit && field.typeCode != 35
                                     && storage_width(field.typeCode) == 0;
        const bool structural = nestedSchemaKind || selected || zeroBit || unknownOptional;
        if (!nestedSchemaKind && field.nestedSchemaRow != runtime::kAbsentRuntimeRow) {
            return RuntimeWalkStatus::unsupportedField;
        }
        for (std::uint32_t index = 0; index < repeats; ++index) {
            if (!step()) {
                return RuntimeWalkStatus::unsafeCount;
            }
            const std::uint32_t occurrence = next_occurrence(field.row);
            if (walk_.overflowed()) {
                return RuntimeWalkStatus::unsafeCount;
            }
            RuntimeAuthoredValue value{};
            bool hasScalar = false;
            if (field.presence != 0) {
                const ValueRole presenceRole =
                    structural ? ValueRole::fieldPresence : ValueRole::scalar;
                if (!source_.take(owner, field, occurrence, presenceRole, value)) {
                    return RuntimeWalkStatus::missingValue;
                }
                if (structural && value.kind != ValueKind::boolean) {
                    return RuntimeWalkStatus::unsupportedField;
                }
                hasScalar = !structural;
                if (hasScalar && !valid_runtime_scalar(field, value)) {
                    return RuntimeWalkStatus::unsupportedField;
                }
                if (!writer_.write(value.present ? 1U : 0U, 1)) {
                    return RuntimeWalkStatus::outputTooSmall;
                }
                if (!value.present) {
                    continue;
                }
            }
            if (unknownOptional) {
                return RuntimeWalkStatus::unsupportedField;
            }
            if (nested) {
                const RuntimeWalkStatus status = walk_nested(owner, field, memory, depth);
                if (status != RuntimeWalkStatus::complete) {
                    return status;
                }
                continue;
            }
            if (selected) {
                const RuntimeWalkStatus status = write_selected(owner, field, occurrence, depth);
                if (status != RuntimeWalkStatus::complete) {
                    return status;
                }
                continue;
            }
            if (command) {
                const RuntimeWalkStatus status = write_command(owner, field, occurrence, depth);
                if (status != RuntimeWalkStatus::complete) {
                    return status;
                }
                continue;
            }
            if (entityReference) {
                const RuntimeWalkStatus status = write_entity_reference(owner, field, occurrence);
                if (status != RuntimeWalkStatus::complete) {
                    return status;
                }
                continue;
            }
            if (schemaReference) {
                const RuntimeWalkStatus status = write_schema_reference(owner, field, occurrence);
                if (status != RuntimeWalkStatus::complete) {
                    return status;
                }
                continue;
            }
            if (zeroBit) {
                continue;
            }
            if (custom) {
                const RuntimeWalkStatus status =
                    write_inline_type(owner, field, occurrence, memory, depth);
                if (status != RuntimeWalkStatus::complete) {
                    return status;
                }
                continue;
            }
            if (!hasScalar && !source_.take(owner, field, occurrence, ValueRole::scalar, value)) {
                return RuntimeWalkStatus::missingValue;
            }
            if (!value.present && field.presence == 0) {
                return RuntimeWalkStatus::missingValue;
            }
            const RuntimeWalkStatus status = write_scalar(field, value, memory);
            if (status != RuntimeWalkStatus::complete) {
                return status;
            }
        }
        return RuntimeWalkStatus::complete;
    }

    const RuntimeSchemaResolver& resolver_;
    bits::Writer& writer_;
    Source& source_;
    RuntimeWalkState walk_{};
};

} // namespace sunrise::middleware::bap::activity_message::wire_schema
