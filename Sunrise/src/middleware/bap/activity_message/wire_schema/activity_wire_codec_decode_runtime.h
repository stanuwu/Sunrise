#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "activity_wire_codec_internal.h"

// The selected-schema reader. It walks one resolved SDK schema and hands every scalar it
// decodes to a sink, so a message-bound decode and a standalone decode share one walk.

namespace sunrise::middleware::bap::activity_message::wire_schema {

/** @return One quantized real decoded from its level index. */
[[nodiscard]] inline float decode_runtime_real(std::uint64_t raw,
                                               const runtime::FieldView& field) noexcept {
    const float minimum = std::bit_cast<float>(static_cast<std::uint32_t>(field.biasOrDynamic));
    const float maximum =
        std::bit_cast<float>(static_cast<std::uint32_t>(field.widthOrCountOffset));
    if (field.parameter2 == 0 || field.parameter2 >= 32) {
        return std::bit_cast<float>(static_cast<std::uint32_t>(raw));
    }
    const std::uint64_t levelCount = std::uint64_t{1} << field.parameter2;
    if (raw == 0) {
        return minimum;
    }
    if (raw == levelCount - 1) {
        return maximum;
    }
    const float step = (maximum - minimum) / static_cast<float>(levelCount - 2);
    return static_cast<float>(raw - 1) * step + step * 0.5F + minimum;
}

/** Writes the scalar domain shared by message-bound and standalone selected decodes. */
template <typename Value>
void fill_runtime_value(Value& value,
                        const runtime::FieldView& field,
                        std::uint64_t decoded) noexcept {
    value.unsignedValue = decoded;
    if (field.typeCode == 2) {
        value.kind = ValueKind::boolean;
    } else if (field.typeCode == 11) {
        value.kind = ValueKind::real32;
        value.realValue = decode_runtime_real(decoded, field);
    } else if (signed_type(field.typeCode)) {
        value.kind = ValueKind::signedInteger;
        value.signedValue = sign_extend(decoded, storage_width(field.typeCode));
    } else {
        value.kind = ValueKind::unsignedInteger;
    }
}

/** Writes one decoded scalar, letting a structural role override the field's own value domain. */
template <typename Value>
void fill_runtime_role_value(Value& value,
                             const runtime::FieldView& field,
                             std::uint64_t decoded,
                             ValueRole role) noexcept {
    fill_runtime_value(value, field, decoded);
    if (role == ValueRole::fieldPresence || role == ValueRole::compressedVectorZero
        || role == ValueRole::customIndexShort || role == ValueRole::nullable160Present
        || role == ValueRole::nullable160Compact || role == ValueRole::referenceValuePrimaryPresent
        || role == ValueRole::referenceValuePrimarySentinel
        || role == ValueRole::referenceValuePresent || role == ValueRole::referenceValueCompact
        || role == ValueRole::vectorUniform || role == ValueRole::vectorUniformInteger) {
        value.kind = ValueKind::boolean;
    } else if (role == ValueRole::vectorX || role == ValueRole::vectorY
               || role == ValueRole::vectorZ || role == ValueRole::vectorW) {
        value.kind = ValueKind::real32;
        value.realValue = std::bit_cast<float>(static_cast<std::uint32_t>(decoded));
    }
}

/** Complete proved runtime-reflection reader parameterized only by value retention. */
template <typename Sink> class RuntimeSchemaDecoder final {
public:
    RuntimeSchemaDecoder(const RuntimeSchemaResolver& resolver,
                         RuntimeBitReader& reader,
                         Sink& sink) noexcept
        : resolver_(resolver), reader_(reader), sink_(sink), bitmapBase_(resolver.firstFieldBit) {}

    /** Decodes one root and reports exact value and bit consumption. */
    [[nodiscard]] RuntimeWalkStatus full(const runtime::SchemaView& schema) noexcept {
        return walk_schema(schema, 0);
    }

private:
    /** A nested schema restores its caller's bitmap base on every return path. */
    struct BitmapScope {
        std::uint32_t& target;
        std::uint32_t prior;
        BitmapScope(std::uint32_t& value, std::uint32_t replacement) noexcept
            : target(value), prior(value) {
            target = replacement;
        }
        ~BitmapScope() {
            target = prior;
        }
    };
    [[nodiscard]] bool step() noexcept {
        return walk_.step();
    }

    [[nodiscard]] std::uint32_t next_occurrence(std::uint32_t fieldRow) noexcept {
        return walk_.next_occurrence(fieldRow);
    }

    /** Decodes one struct schema in published field order. */
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

    /** Decodes `count` runtime records of one array; the schema's array length bounds it. */
    [[nodiscard]] RuntimeWalkStatus
    walk_array(const runtime::SchemaView& schema, std::uint32_t count, std::size_t depth) noexcept {
        if (!valid_runtime_schema(schema)) {
            return RuntimeWalkStatus::schemaUnavailable;
        }
        if (schema.arrayLength == 0 || count > schema.arrayLength || !step()) {
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

    /** Applies fixed or decoded count-backed repetition to a nested schema. */
    [[nodiscard]] RuntimeWalkStatus walk_nested(const runtime::SchemaView& owner,
                                                const runtime::FieldView& field,
                                                RuntimeMemoryMap& memory,
                                                std::size_t depth) noexcept {
        if (field.nestedSchemaRow == runtime::kAbsentRuntimeRow
            || resolver_.readSchema == nullptr) {
            return RuntimeWalkStatus::schemaUnavailable;
        }
        runtime::SchemaView nested{};
        if (!resolver_.readSchema(resolver_.context, field.nestedSchemaRow, nested)
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

    /** Decodes a nullable handle followed by its selected schema body. */
    [[nodiscard]] RuntimeWalkStatus walk_selected(const runtime::SchemaView& owner,
                                                  const runtime::FieldView& field,
                                                  std::uint32_t occurrence,
                                                  std::size_t depth) noexcept {
        const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
        std::uint64_t present = 0;
        if (!reader_.read(1, present)) {
            return RuntimeWalkStatus::malformed;
        }
        if (present == 0) {
            sink_.append(owner, field, occurrence, at, 0, 0, false, ValueRole::selectedHandle);
            return RuntimeWalkStatus::complete;
        }
        std::uint64_t rawHandle = 0;
        if (!reader_.read(32, rawHandle)) {
            return RuntimeWalkStatus::malformed;
        }
        sink_.append(
            owner, field, occurrence, at + 1, 32, rawHandle, true, ValueRole::selectedHandle);
        const auto handle = static_cast<std::uint32_t>(rawHandle);
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

    /** Reads one type-36 command and resolves its selector-owned payload schema. */
    [[nodiscard]] RuntimeWalkStatus walk_command(const runtime::SchemaView& owner,
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
            const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
            std::uint64_t value = 0;
            if (!reader_.read(width, value)) {
                return RuntimeWalkStatus::malformed;
            }
            sink_.append(owner, field, occurrence, at, width, value, true, role);
            if (resolver_.nativeCommandEmptyShortcut && role == ValueRole::commandDefault
                && value != 0) {
                return RuntimeWalkStatus::complete;
            }
            if (resolver_.nativeCommandEmptyShortcut && role == ValueRole::commandTargetReference
                && value != 0) {
                return RuntimeWalkStatus::unsupportedField;
            }
            if (role == ValueRole::commandSelector) {
                selector = static_cast<std::uint8_t>(value);
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

    /** Reads one type-18 sentinel or live simulation entity token. */
    [[nodiscard]] RuntimeWalkStatus walk_entity_reference(const runtime::SchemaView& owner,
                                                          const runtime::FieldView& field,
                                                          std::uint32_t occurrence) noexcept {
        const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
        std::uint64_t live = 0;
        if (!reader_.read(1, live)) {
            return RuntimeWalkStatus::malformed;
        }
        if (live == 0) {
            std::uint64_t sentinel = 0;
            if (!reader_.read(1, sentinel)) {
                return RuntimeWalkStatus::malformed;
            }
            sink_.append(
                owner, field, occurrence, at, 2, sentinel, true, ValueRole::entityReferenceKind);
            return RuntimeWalkStatus::complete;
        }
        std::uint64_t slot = 0;
        std::uint64_t incarnation = 0;
        if (!reader_.read(13, slot) || !reader_.read(4, incarnation)) {
            return RuntimeWalkStatus::malformed;
        }
        sink_.append(owner, field, occurrence, at, 1, 2, true, ValueRole::entityReferenceKind);
        sink_.append(
            owner, field, occurrence, at + 1, 13, slot, true, ValueRole::entityReferenceSlot);
        sink_.append(owner,
                     field,
                     occurrence,
                     at + 14,
                     4,
                     incarnation,
                     true,
                     ValueRole::entityReferenceIncarnation);
        return RuntimeWalkStatus::complete;
    }

    /** Reads one type-23 nullable runtime schema handle. */
    [[nodiscard]] RuntimeWalkStatus walk_schema_reference(const runtime::SchemaView& owner,
                                                          const runtime::FieldView& field,
                                                          std::uint32_t occurrence) noexcept {
        const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
        std::uint64_t present = 0;
        if (!reader_.read(1, present)) {
            return RuntimeWalkStatus::malformed;
        }
        if (present == 0) {
            sink_.append(owner, field, occurrence, at, 0, 0, false, ValueRole::schemaReference);
            return RuntimeWalkStatus::complete;
        }
        std::uint64_t handle = 0;
        if (!reader_.read(32, handle)) {
            return RuntimeWalkStatus::malformed;
        }
        sink_.append(
            owner, field, occurrence, at + 1, 32, handle, true, ValueRole::schemaReference);
        return RuntimeWalkStatus::complete;
    }

    /** Types 13 and 39 share raw floats and cell-profile quantized codes. */
    [[nodiscard]] RuntimeWalkStatus walk_vector3(const runtime::SchemaView& owner,
                                                 const runtime::FieldView& field,
                                                 std::uint32_t occurrence) noexcept {
        const auto* profile = resolver_.positionProfile;
        bool compressed = false;
        if (profile != nullptr && profile->selectorPresent) {
            const auto at = static_cast<std::uint32_t>(reader_.position());
            std::uint64_t value = 0;
            if (!reader_.read(1, value)) {
                return RuntimeWalkStatus::malformed;
            }
            compressed = value != 0;
            sink_.append(
                owner, field, occurrence, at, 1, value, true, ValueRole::positionCompressed);
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
                const auto at = static_cast<std::uint32_t>(reader_.position());
                std::uint64_t value = 0;
                if (!reader_.read(width, value)) {
                    return RuntimeWalkStatus::malformed;
                }
                sink_.append(owner, field, occurrence, at, width, value, true, roles[index]);
            }
            return RuntimeWalkStatus::complete;
        }
        for (const ValueRole role : {ValueRole::vectorX, ValueRole::vectorY, ValueRole::vectorZ}) {
            const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
            std::uint64_t value = 0;
            if (!reader_.read(32, value)) {
                return RuntimeWalkStatus::malformed;
            }
            sink_.append(owner, field, occurrence, at, 32, value, true, role);
        }
        return RuntimeWalkStatus::complete;
    }

    /** Type 15 distinguishes points, raw directions, and positions with explicit W. */
    [[nodiscard]] RuntimeWalkStatus walk_vector4(const runtime::SchemaView& owner,
                                                 const runtime::FieldView& field,
                                                 std::uint32_t occurrence) noexcept {
        const auto at = static_cast<std::uint32_t>(reader_.position());
        std::uint64_t point = 0, direction = 0;
        if (!reader_.read(1, point)) {
            return RuntimeWalkStatus::malformed;
        }
        sink_.append(owner, field, occurrence, at, 1, point, true, ValueRole::positionPoint);
        if (point == 0) {
            if (!reader_.read(1, direction)) {
                return RuntimeWalkStatus::malformed;
            }
            sink_.append(
                owner, field, occurrence, at + 1, 1, direction, true, ValueRole::positionDirection);
        }
        if (direction != 0) {
            for (const auto role : {ValueRole::vectorX, ValueRole::vectorY, ValueRole::vectorZ}) {
                const auto position = static_cast<std::uint32_t>(reader_.position());
                std::uint64_t raw = 0;
                if (!reader_.read(32, raw)) {
                    return RuntimeWalkStatus::malformed;
                }
                sink_.append(owner, field, occurrence, position, 32, raw, true, role);
            }
        } else {
            const auto status = walk_vector3(owner, field, occurrence);
            if (status != RuntimeWalkStatus::complete) {
                return status;
            }
        }
        if (point == 0 && direction == 0) {
            const auto position = static_cast<std::uint32_t>(reader_.position());
            std::uint64_t raw = 0;
            if (!reader_.read(32, raw)) {
                return RuntimeWalkStatus::malformed;
            }
            sink_.append(owner, field, occurrence, position, 32, raw, true, ValueRole::vectorW);
        }
        return RuntimeWalkStatus::complete;
    }

    /** Type 14 exposes structural codes so replay does not lose precision. */
    [[nodiscard]] RuntimeWalkStatus walk_compressed_vector(const runtime::SchemaView& owner,
                                                           const runtime::FieldView& field,
                                                           std::uint32_t occurrence) noexcept {
        const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
        std::uint64_t zero = 0;
        if (!reader_.read(1, zero)) {
            return RuntimeWalkStatus::malformed;
        }
        sink_.append(owner, field, occurrence, at, 1, zero, true, ValueRole::compressedVectorZero);
        if (zero != 0) {
            return RuntimeWalkStatus::complete;
        }
        std::uint64_t direction = 0;
        std::uint64_t magnitude = 0;
        if (!reader_.read(19, direction) || !reader_.read(16, magnitude)) {
            return RuntimeWalkStatus::malformed;
        }
        sink_.append(owner,
                     field,
                     occurrence,
                     at + 1,
                     19,
                     direction,
                     true,
                     ValueRole::compressedVectorDirection);
        sink_.append(owner,
                     field,
                     occurrence,
                     at + 20,
                     16,
                     magnitude,
                     true,
                     ValueRole::compressedVectorMagnitude);
        return RuntimeWalkStatus::complete;
    }

    /** Retains the exact type-26 nullable record and its compact-width branch. */
    [[nodiscard]] RuntimeWalkStatus walk_nullable160(const runtime::SchemaView& owner,
                                                     const runtime::FieldView& field,
                                                     std::uint32_t occurrence) noexcept {
        const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
        std::uint64_t present = 0;
        if (!reader_.read(1, present)) {
            return RuntimeWalkStatus::malformed;
        }
        sink_.append(owner, field, occurrence, at, 1, present, true, ValueRole::nullable160Present);
        if (present == 0) {
            return RuntimeWalkStatus::complete;
        }
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        std::uint64_t compact = 0;
        if (!reader_.read(32, a) || !reader_.read(32, b) || !reader_.read(1, compact)) {
            return RuntimeWalkStatus::malformed;
        }
        const std::uint8_t cWidth = compact != 0 ? 12 : 32;
        std::uint64_t c = 0;
        if (!reader_.read(cWidth, c)) {
            return RuntimeWalkStatus::malformed;
        }
        sink_.append(owner, field, occurrence, at + 1, 32, a, true, ValueRole::nullable160A);
        sink_.append(owner, field, occurrence, at + 33, 32, b, true, ValueRole::nullable160B);
        sink_.append(
            owner, field, occurrence, at + 65, 1, compact, true, ValueRole::nullable160Compact);
        sink_.append(
            owner, field, occurrence, at + 66, cWidth, c, true, ValueRole::nullable160RawC);
        return RuntimeWalkStatus::complete;
    }

    /** Retains the exact absent sentinel or live entity/depth token used by type 19. */
    [[nodiscard]] RuntimeWalkStatus walk_primary_reference(const runtime::SchemaView& owner,
                                                           const runtime::FieldView& field,
                                                           std::uint32_t occurrence) noexcept {
        const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
        std::uint64_t primaryPresent = 0;
        if (!reader_.read(1, primaryPresent)) {
            return RuntimeWalkStatus::malformed;
        }
        sink_.append(owner,
                     field,
                     occurrence,
                     at,
                     1,
                     primaryPresent,
                     true,
                     ValueRole::referenceValuePrimaryPresent);
        if (primaryPresent == 0) {
            std::uint64_t sentinel = 0;
            if (!reader_.read(1, sentinel)) {
                return RuntimeWalkStatus::malformed;
            }
            sink_.append(owner,
                         field,
                         occurrence,
                         at + 1,
                         1,
                         sentinel,
                         true,
                         ValueRole::referenceValuePrimarySentinel);
        } else {
            std::uint64_t slot = 0;
            std::uint64_t incarnation = 0;
            std::uint64_t depth = 0;
            if (!reader_.read(13, slot) || !reader_.read(4, incarnation)
                || !reader_.read(6, depth)) {
                return RuntimeWalkStatus::malformed;
            }
            sink_.append(owner,
                         field,
                         occurrence,
                         at + 1,
                         13,
                         slot,
                         true,
                         ValueRole::referenceValuePrimarySlot);
            sink_.append(owner,
                         field,
                         occurrence,
                         at + 14,
                         4,
                         incarnation,
                         true,
                         ValueRole::referenceValuePrimaryIncarnation);
            sink_.append(owner,
                         field,
                         occurrence,
                         at + 18,
                         6,
                         depth,
                         true,
                         ValueRole::referenceValuePrimaryDepth);
        }
        return RuntimeWalkStatus::complete;
    }

    /** Retains both references and the compact index branch of type 43. */
    [[nodiscard]] RuntimeWalkStatus
    walk_reference_value_variant(const runtime::SchemaView& owner,
                                 const runtime::FieldView& field,
                                 std::uint32_t occurrence) noexcept {
        const RuntimeWalkStatus primary = walk_primary_reference(owner, field, occurrence);
        if (primary != RuntimeWalkStatus::complete) {
            return primary;
        }
        return walk_reference_value(owner, field, occurrence);
    }

    /** Types 27 and 43 share a nullable pair of tags followed by a sized index. */
    [[nodiscard]] RuntimeWalkStatus walk_reference_value(const runtime::SchemaView& owner,
                                                         const runtime::FieldView& field,
                                                         std::uint32_t occurrence) noexcept {
        const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
        std::uint64_t valuePresent = 0;
        if (!reader_.read(1, valuePresent)) {
            return RuntimeWalkStatus::malformed;
        }
        sink_.append(
            owner, field, occurrence, at, 1, valuePresent, true, ValueRole::referenceValuePresent);
        if (valuePresent == 0) {
            return RuntimeWalkStatus::complete;
        }
        std::uint64_t tagA = 0;
        std::uint64_t tagB = 0;
        std::uint64_t compact = 0;
        if (!reader_.read(32, tagA) || !reader_.read(32, tagB) || !reader_.read(1, compact)) {
            return RuntimeWalkStatus::malformed;
        }
        const std::uint8_t indexWidth = compact != 0 ? 12 : 32;
        std::uint64_t index = 0;
        if (!reader_.read(indexWidth, index)) {
            return RuntimeWalkStatus::malformed;
        }
        sink_.append(
            owner, field, occurrence, at + 1, 32, tagA, true, ValueRole::referenceValueTagA);
        sink_.append(
            owner, field, occurrence, at + 33, 32, tagB, true, ValueRole::referenceValueTagB);
        sink_.append(
            owner, field, occurrence, at + 65, 1, compact, true, ValueRole::referenceValueCompact);
        sink_.append(owner,
                     field,
                     occurrence,
                     at + 66,
                     indexWidth,
                     index,
                     true,
                     ValueRole::referenceValueIndex);
        return RuntimeWalkStatus::complete;
    }

    /** Universal variant arms recurse only through registered runtime types. */
    [[nodiscard]] RuntimeWalkStatus walk_inline_type(const runtime::SchemaView& owner,
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
            return walk_vector3(owner, field, occurrence);
        }
        if (field.typeCode == 16) {
            const auto at = static_cast<std::uint32_t>(reader_.position());
            std::uint64_t shortcut = 0, axis = 0, angle = 0;
            if (!reader_.read(1, shortcut)) {
                return RuntimeWalkStatus::malformed;
            }
            const std::uint8_t axisWidth = shortcut != 0 ? 1 : 19;
            if (!reader_.read(axisWidth, axis) || !reader_.read(7, angle)) {
                return RuntimeWalkStatus::malformed;
            }
            sink_.append(
                owner, field, occurrence, at, 1, shortcut, true, ValueRole::rotationAxisShortcut);
            sink_.append(owner,
                         field,
                         occurrence,
                         at + 1,
                         axisWidth,
                         axis,
                         true,
                         ValueRole::rotationAxisCode);
            sink_.append(owner,
                         field,
                         occurrence,
                         at + 1 + axisWidth,
                         7,
                         angle,
                         true,
                         ValueRole::rotationAngleCode);
            return RuntimeWalkStatus::complete;
        }
        if (field.typeCode == 42) {
            const auto at = static_cast<std::uint32_t>(reader_.position());
            std::uint64_t uniform = 0;
            if (!reader_.read(1, uniform)) {
                return RuntimeWalkStatus::malformed;
            }
            sink_.append(owner, field, occurrence, at, 1, uniform, true, ValueRole::vectorUniform);
            const bool quantized = field.parameter2 > 0 && field.parameter2 < 32;
            unsigned width = quantized ? field.parameter2 : 32;
            if (uniform != 0 && quantized) {
                std::uint64_t integer = 0;
                if (!reader_.read(1, integer)) {
                    return RuntimeWalkStatus::malformed;
                }
                sink_.append(owner,
                             field,
                             occurrence,
                             at + 1,
                             1,
                             integer,
                             true,
                             ValueRole::vectorUniformInteger);
                if (integer != 0) {
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
            for (unsigned index = 0; index < (uniform != 0 ? 1U : 4U); ++index) {
                const auto offset = static_cast<std::uint32_t>(reader_.position());
                std::uint64_t code = 0;
                if (width != 0 && !reader_.read(static_cast<std::uint8_t>(width), code)) {
                    return RuntimeWalkStatus::malformed;
                }
                sink_.append(owner,
                             field,
                             occurrence,
                             offset,
                             static_cast<std::uint8_t>(width),
                             code,
                             true,
                             roles[index]);
            }
            return RuntimeWalkStatus::complete;
        }
        if (field.typeCode == 44 || field.typeCode == 45) {
            const auto width =
                field.typeCode == 45
                    ? (field.parameter2 > 0 && field.parameter2 < 32 ? field.parameter2 : 32)
                    : field.widthOrCountOffset;
            const auto at = static_cast<std::uint32_t>(reader_.position());
            std::uint64_t code = 0;
            if (width <= 0 || width > 32 || !reader_.read(static_cast<std::uint8_t>(width), code)) {
                return RuntimeWalkStatus::malformed;
            }
            sink_.append(owner,
                         field,
                         occurrence,
                         at,
                         static_cast<std::uint8_t>(width),
                         code,
                         true,
                         ValueRole::opaqueScalarCode);
            return RuntimeWalkStatus::complete;
        }
        if (field.typeCode == 14 || field.typeCode == 40) {
            return walk_compressed_vector(owner, field, occurrence);
        }
        if (field.typeCode == 15) {
            return walk_vector4(owner, field, occurrence);
        }
        if (field.typeCode == 18) {
            return walk_entity_reference(owner, field, occurrence);
        }
        if (field.typeCode == 19) {
            return walk_primary_reference(owner, field, occurrence);
        }
        if (field.typeCode == 22 || field.typeCode == 23) {
            return walk_schema_reference(owner, field, occurrence);
        }
        if (field.typeCode == 26) {
            return walk_nullable160(owner, field, occurrence);
        }
        if (field.typeCode == 27) {
            return walk_reference_value(owner, field, occurrence);
        }
        if (field.typeCode == 43) {
            return walk_reference_value_variant(owner, field, occurrence);
        }
        if (field.typeCode == 24 || field.typeCode == 25) {
            const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
            std::uint64_t selector = 0;
            if (!reader_.read(6, selector) || resolver_.validateType == nullptr
                || !resolver_.validateType(resolver_.context,
                                           static_cast<std::uint8_t>(selector))) {
                return RuntimeWalkStatus::unsupportedField;
            }
            sink_.append(
                owner, field, occurrence, at, 6, selector, true, ValueRole::variantSelector);
            field.typeCode = static_cast<std::uint8_t>(selector);
            if (resolver_.isZeroBitType != nullptr
                && resolver_.isZeroBitType(resolver_.context, field.typeCode)) {
                return RuntimeWalkStatus::complete;
            }
            return walk_inline_type(owner, field, occurrence, memory, depth + 1);
        }
        if (field.typeCode == 28) {
            runtime::FieldView variant = field;
            variant.typeCode = 25;
            const RuntimeWalkStatus selected =
                walk_inline_type(owner, variant, occurrence, memory, depth + 1);
            if (selected != RuntimeWalkStatus::complete) {
                return selected;
            }
            const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
            std::uint64_t shortForm = 0;
            if (!reader_.read(1, shortForm)) {
                return RuntimeWalkStatus::malformed;
            }
            std::uint64_t index = 0;
            if (!reader_.read(shortForm != 0 ? 12 : 32, index)) {
                return RuntimeWalkStatus::malformed;
            }
            sink_.append(
                owner, field, occurrence, at, 1, shortForm, true, ValueRole::customIndexShort);
            sink_.append(owner,
                         field,
                         occurrence,
                         at + 1,
                         shortForm != 0 ? 12 : 32,
                         index,
                         true,
                         ValueRole::customIndex);
            return RuntimeWalkStatus::complete;
        }
        const bool raw64 = field.typeCode == 35;
        const std::uint8_t nativeWidth = raw64 ? 64 : storage_width(field.typeCode);
        const std::uint8_t width = raw64 ? 64 : runtime_declared_width(field);
        if (nativeWidth == 0 || width == 0) {
            return RuntimeWalkStatus::unsupportedField;
        }
        const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
        std::uint64_t raw = 0;
        if (!reader_.read(width, raw)) {
            return RuntimeWalkStatus::malformed;
        }
        const std::uint64_t decoded =
            raw64 || field.typeCode == 11 || field.typeCode == 2
                ? raw
                : (raw - static_cast<std::uint64_t>(static_cast<std::int64_t>(field.biasOrDynamic)))
                      & mask(nativeWidth);
        sink_.append(owner, field, occurrence, at, width, decoded, true, ValueRole::scalar);
        return RuntimeWalkStatus::complete;
    }

    /** Decodes every occurrence of one reflected field with exact offsets. */
    [[nodiscard]] RuntimeWalkStatus walk_field(const runtime::SchemaView& owner,
                                               const runtime::FieldView& field,
                                               std::uint32_t repeats,
                                               RuntimeMemoryMap& memory,
                                               std::size_t depth) noexcept {
        if (!step()) {
            return RuntimeWalkStatus::unsafeCount;
        }
        if (field.row < owner.firstField || field.row >= owner.firstField + owner.fieldCount
            || field.presence > 1) {
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
        const bool raw64 = field.typeCode == 35;
        const bool zeroBit = resolver_.isZeroBitType != nullptr
                             && resolver_.isZeroBitType(resolver_.context, field.typeCode);
        // A selected field carries its schema handle on the wire, so it is structural but must
        // not also declare a nested schema row.
        const bool nestedSchemaKind =
            nested || command || entityReference || schemaReference || custom;
        const bool unknownOptional = field.presence != 0 && !nestedSchemaKind && !selected
                                     && !zeroBit && !raw64 && storage_width(field.typeCode) == 0;
        const bool structural = nestedSchemaKind || selected || zeroBit || unknownOptional;
        if (!nestedSchemaKind && field.nestedSchemaRow != runtime::kAbsentRuntimeRow) {
            return RuntimeWalkStatus::unsupportedField;
        }
        const std::uint8_t nativeWidth = structural || raw64 ? 0 : storage_width(field.typeCode);
        const std::uint8_t declaredWidth = structural || raw64 ? 0 : runtime_declared_width(field);
        if (!structural && !raw64 && (nativeWidth == 0 || declaredWidth == 0)) {
            return RuntimeWalkStatus::unsupportedField;
        }
        for (std::uint32_t index = 0; index < repeats; ++index) {
            const std::uint64_t relative = static_cast<std::uint64_t>(field.bitmapOffset)
                                           * (owner.arrayLength != 0 ? index : 1U);
            if (relative > (std::numeric_limits<std::uint32_t>::max)() - bitmapBase_) {
                return RuntimeWalkStatus::unsafeCount;
            }
            const auto fieldBit = bitmapBase_ + static_cast<std::uint32_t>(relative);
            if (resolver_.recordPresence != nullptr && (field.presence != 0 || nested)
                && !field.hasBitmapOffset) {
                return RuntimeWalkStatus::schemaUnavailable;
            }
            BitmapScope bitmapScope(bitmapBase_, fieldBit + (field.presence != 0 ? 1U : 0U));
            if (!step()) {
                return RuntimeWalkStatus::unsafeCount;
            }
            const std::uint32_t occurrence = next_occurrence(field.row);
            if (walk_.overflowed()) {
                return RuntimeWalkStatus::unsafeCount;
            }
            if (field.presence != 0) {
                const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
                std::uint64_t present = 0;
                if (!reader_.read(1, present)) {
                    return RuntimeWalkStatus::malformed;
                }
                if (resolver_.recordPresence != nullptr
                    && !resolver_.recordPresence(resolver_.context, fieldBit, present != 0)) {
                    return RuntimeWalkStatus::unsafeCount;
                }
                if (structural) {
                    sink_.append(owner,
                                 field,
                                 occurrence,
                                 at,
                                 1,
                                 present,
                                 present != 0,
                                 ValueRole::fieldPresence);
                }
                if (present == 0) {
                    if (!structural) {
                        sink_.append(owner, field, occurrence, at, 0, 0, false, ValueRole::scalar);
                    }
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
                const RuntimeWalkStatus status = walk_selected(owner, field, occurrence, depth);
                if (status != RuntimeWalkStatus::complete) {
                    return status;
                }
                continue;
            }
            if (command) {
                const RuntimeWalkStatus status = walk_command(owner, field, occurrence, depth);
                if (status != RuntimeWalkStatus::complete) {
                    return status;
                }
                continue;
            }
            if (entityReference) {
                const RuntimeWalkStatus status = walk_entity_reference(owner, field, occurrence);
                if (status != RuntimeWalkStatus::complete) {
                    return status;
                }
                continue;
            }
            if (schemaReference) {
                const RuntimeWalkStatus status = walk_schema_reference(owner, field, occurrence);
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
                    walk_inline_type(owner, field, occurrence, memory, depth);
                if (status != RuntimeWalkStatus::complete) {
                    return status;
                }
                continue;
            }
            const std::uint8_t width = raw64 ? 64 : declaredWidth;
            const std::uint32_t at = static_cast<std::uint32_t>(reader_.position());
            std::uint64_t raw = 0;
            if (!reader_.read(width, raw)) {
                return RuntimeWalkStatus::malformed;
            }
            const std::uint64_t decoded =
                raw64 || field.typeCode == 11 || field.typeCode == 2
                    ? raw
                    : (raw
                       - static_cast<std::uint64_t>(static_cast<std::int64_t>(field.biasOrDynamic)))
                          & mask(nativeWidth);
            sink_.append(owner, field, occurrence, at, width, decoded, true, ValueRole::scalar);
            if (!raw64) {
                const std::int64_t memoryValue = signed_type(field.typeCode)
                                                     ? sign_extend(decoded, nativeWidth)
                                                     : static_cast<std::int64_t>(decoded);
                memory.store(field.structOffset, memoryValue);
            }
        }
        return RuntimeWalkStatus::complete;
    }

    const RuntimeSchemaResolver& resolver_;
    RuntimeBitReader& reader_;
    Sink& sink_;
    std::uint32_t bitmapBase_{};
    RuntimeWalkState walk_{};
};

} // namespace sunrise::middleware::bap::activity_message::wire_schema
