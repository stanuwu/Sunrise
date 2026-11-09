#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../encoding/bit_reader.h"
#include "activity_wire_schema.h"

namespace sunrise::middleware::bap::activity_message::wire_schema {

/** Maximum scalar values retained for one decoded panel row. */
inline constexpr std::size_t kDecodedValueCapacity = 512;
/** Maximum edited field/element pairs in one authored draft. */
inline constexpr std::size_t kDraftValueCapacity = 512;
/** Maximum values visited by one runtime-selected schema walk. */
inline constexpr std::size_t kRuntimeValueCapacity = 1024;

namespace runtime {

/** Row index meaning "no row"; the SDK never numbers a real row this high. */
inline constexpr std::uint32_t kAbsentRuntimeRow = 0xFFFFFFFFU;
/** Field type 1 walks one nested schema row. */
inline constexpr std::uint8_t kNestedType = 1;
/** Field type 34 carries its schema handle and reads through the selected-schema reader. */
inline constexpr std::uint8_t kSelectedType = 34;
/** Field types 12 and 35 are raw 64-bit words, read and written whole. */
inline constexpr std::uint8_t kRaw64AliasType = 12;
inline constexpr std::uint8_t kRaw64Type = 35;

/** One runtime schema row supplied by the generated Activity SDK. */
struct SchemaView final {
    std::uint32_t row{kAbsentRuntimeRow};
    std::uint32_t handle{};
    std::uint32_t arrayLength{};
    std::uint32_t firstField{kAbsentRuntimeRow};
    std::uint32_t fieldCount{};
    std::uint32_t structSize{};
};

/** One runtime reflection field supplied by the generated Activity SDK. */
struct FieldView final {
    std::uint32_t row{kAbsentRuntimeRow};
    std::uint32_t nestedSchemaRow{kAbsentRuntimeRow};
    std::uint32_t structOffset{};
    std::int32_t biasOrDynamic{};
    std::int32_t widthOrCountOffset{};
    std::uint8_t typeCode{};
    std::uint8_t presence{};
    std::uint8_t parameter2{};
    std::uint32_t bitmapOffset{};
    bool hasBitmapOffset{};
};

} // namespace runtime

enum class ValueKind : std::uint8_t {
    unsignedInteger,
    signedInteger,
    boolean,
    real32,
};

/** Identifies structural values which share one reflected field row. */
enum class ValueRole : std::uint8_t {
    scalar,
    fieldPresence,
    selectedHandle,
    commandDefault,
    commandMode,
    commandSelector,
    commandTargetReference,
    commandTarget,
    commandAuxiliary16,
    commandAuxiliary8,
    entityReferenceKind,
    entityReferenceSlot,
    entityReferenceIncarnation,
    schemaReference,
    vectorX,
    vectorY,
    vectorZ,
    vectorW,
    variantSelector,
    compressedVectorZero,
    compressedVectorDirection,
    compressedVectorMagnitude,
    customIndexShort,
    customIndex,
    nullable160Present,
    nullable160A,
    nullable160B,
    nullable160Compact,
    nullable160RawC,
    referenceValuePrimaryPresent,
    referenceValuePrimarySentinel,
    referenceValuePrimarySlot,
    referenceValuePrimaryIncarnation,
    referenceValuePrimaryDepth,
    referenceValuePresent,
    referenceValueTagA,
    referenceValueTagB,
    referenceValueCompact,
    referenceValueIndex,
    positionCompressed,
    positionCodeX,
    positionCodeY,
    positionCodeZ,
    positionPoint,
    positionDirection,
    opaqueScalarCode,
    vectorUniform,
    vectorUniformInteger,
    vectorCodeX,
    vectorCodeY,
    vectorCodeZ,
    vectorCodeW,
    rotationAxisShortcut,
    rotationAxisCode,
    rotationAngleCode,
};

/** Position widths belong to the selected cell, independently of the reflection codec family. */
struct PositionProfile final {
    std::array<std::uint8_t, 3> axisBits{};
    bool selectorPresent{};
    bool hasWidths{};
};

using FindRuntimeSchema = bool (*)(const void*, std::uint32_t, runtime::SchemaView&) noexcept;
using ReadRuntimeSchema = bool (*)(const void*, std::uint32_t, runtime::SchemaView&) noexcept;
using ReadRuntimeField = bool (*)(const void*, std::uint32_t, runtime::FieldView&) noexcept;
using ResolveCommandPayload = bool (*)(const void*, std::uint8_t, std::uint32_t&) noexcept;
using ValidateRuntimeType = bool (*)(const void*, std::uint8_t) noexcept;
using IsZeroBitRuntimeType = bool (*)(const void*, std::uint8_t) noexcept;

/** Borrowed adapters for runtime-selected reflection and command payload schemas. */
struct RuntimeSchemaResolver final {
    const void* context{};
    FindRuntimeSchema findSchema{};
    ReadRuntimeSchema readSchema{};
    ReadRuntimeField readField{};
    ResolveCommandPayload resolveCommandPayload{};
    ValidateRuntimeType validateType{};
    IsZeroBitRuntimeType isZeroBitType{};
    const PositionProfile* positionProfile{};
    bool (*recordPresence)(const void*, std::uint32_t, bool) noexcept {};
    std::uint32_t firstFieldBit{};
    std::uint8_t (*canonicalType)(const void*, std::uint8_t) noexcept {};
    bool nativeCommandEmptyShortcut{};
};

/** Why one codec run stopped: complete, or the exact input it still needs. */
enum class CodecStatus : std::uint8_t {
    complete,
    completeWithPadding,
    needsPackageSchema,
    needsRuntimeSchema,
    mixedRawTail,
    noDefinition,
    unsupportedField,
    unsafeCount,
    malformed,
    outputTooSmall,
};

/** One decoded scalar. Repeated fields use element to identify the array position. */
struct DecodedValue final {
    std::uint16_t fieldIndex{};
    /** Flattened occurrence within this field, including enclosing repeated records. */
    std::uint32_t element{};
    std::uint32_t bitOffset{};
    std::uint8_t width{};
    std::uint32_t selectedSchemaHandle{kAbsentUnsigned};
    std::uint32_t selectedSchemaRow{kAbsentUnsigned};
    std::uint32_t selectedFieldRow{kAbsentUnsigned};
    std::uint32_t selectedOccurrence{};
    ValueRole role{ValueRole::scalar};
    ValueKind kind{ValueKind::unsignedInteger};
    std::uint64_t unsignedValue{};
    std::int64_t signedValue{};
    float realValue{};
    bool present{true};
};

/** Bounded result used directly by the packet inspector. */
struct DecodedPacket final {
    std::array<DecodedValue, kDecodedValueCapacity> values{};
    std::size_t valueCount{};
    std::size_t bitsConsumed{};
    std::size_t bitsRemaining{};
    std::uint32_t selectedSchema{};
    CodecStatus status{CodecStatus::malformed};
    bool rootPresent{true};
    bool valuesTruncated{};
    bool valuesRedacted{};
};

/** One decoded-value draft used by the generic reflection encoder. */
struct DraftValue final {
    std::uint16_t fieldIndex{};
    /** Flattened occurrence within this field, including enclosing repeated records. */
    std::uint32_t element{};
    std::uint32_t selectedSchemaHandle{kAbsentUnsigned};
    std::uint32_t selectedSchemaRow{kAbsentUnsigned};
    std::uint32_t selectedFieldRow{kAbsentUnsigned};
    std::uint32_t selectedOccurrence{};
    ValueRole role{ValueRole::scalar};
    ValueKind kind{ValueKind::unsignedInteger};
    std::uint64_t unsignedValue{};
    std::int64_t signedValue{};
    float realValue{};
    bool present{};
    bool assigned{};
};

/** One explicit value used to author a complete runtime schema body. */
struct RuntimeDraftValue {
    std::uint32_t schemaHandle{};
    std::uint32_t schemaRow{kAbsentUnsigned};
    std::uint32_t fieldRow{kAbsentUnsigned};
    std::uint32_t occurrence{};
    ValueRole role{ValueRole::scalar};
    ValueKind kind{ValueKind::unsignedInteger};
    std::uint64_t unsignedValue{};
    std::int64_t signedValue{};
    float realValue{};
    bool present{};
};

/** One decoded value from a complete runtime schema body. */
struct RuntimeDecodedValue final : RuntimeDraftValue {
    std::uint32_t bitOffset{};
    std::uint8_t width{};
};

/** Bounded result from one complete runtime schema decode. */
struct RuntimeDecodeResult final {
    std::size_t valueCount{};
    std::size_t requiredValueCount{};
    std::size_t bitsConsumed{};
    std::size_t bitsRemaining{};
    CodecStatus status{CodecStatus::malformed};
    bool valuesTruncated{};
};

/** Fixed-capacity authoring state owned by the UI. */
struct PacketDraft final {
    std::array<DraftValue, kDraftValueCapacity> values{};
    std::size_t valueCount{};
    bool rootPresent{true};
};

/** Decodes a reflection packet and retains named scalar values. */
[[nodiscard]] bool decode(const MessageDescriptor& message,
                          std::span<const std::byte> payload,
                          DecodedPacket& output) noexcept;

[[nodiscard]] bool decode(const MessageDescriptor& message,
                          std::span<const std::byte> payload,
                          const RuntimeSchemaResolver& resolver,
                          DecodedPacket& output) noexcept;

[[nodiscard]] bool decode_full_schema(std::uint32_t schemaHandle,
                                      std::span<const std::byte> payload,
                                      std::size_t bitCount,
                                      const RuntimeSchemaResolver& resolver,
                                      std::span<RuntimeDecodedValue> values,
                                      RuntimeDecodeResult& result) noexcept;

/** Decodes one runtime schema prefix and leaves later fields in the caller's reader. */
[[nodiscard]] bool decode_full_schema_prefix(std::uint32_t schemaHandle,
                                             middleware::encoding::bits::Reader& reader,
                                             const RuntimeSchemaResolver& resolver,
                                             std::span<RuntimeDecodedValue> values,
                                             RuntimeDecodeResult& result) noexcept;

/** Encodes a reflection packet from decoded values. Missing required values use zero. */
[[nodiscard]] bool encode(const MessageDescriptor& message,
                          const PacketDraft& draft,
                          std::span<std::byte> output,
                          std::size_t& written,
                          std::size_t& writtenBits,
                          CodecStatus& status) noexcept;

[[nodiscard]] bool encode(const MessageDescriptor& message,
                          const PacketDraft& draft,
                          const RuntimeSchemaResolver& resolver,
                          std::span<std::byte> output,
                          std::size_t& written,
                          std::size_t& writtenBits,
                          CodecStatus& status) noexcept;

[[nodiscard]] bool encode_full_schema(std::uint32_t schemaHandle,
                                      std::span<const RuntimeDraftValue> values,
                                      const RuntimeSchemaResolver& resolver,
                                      std::span<std::byte> output,
                                      std::size_t& written,
                                      std::size_t& writtenBits,
                                      CodecStatus& status) noexcept;

/** Finds or creates one field/element draft row. */
[[nodiscard]] DraftValue*
edit(PacketDraft& draft, std::uint16_t fieldIndex, std::uint32_t element) noexcept;

/** Finds or creates one structural role on an outer field occurrence. */
[[nodiscard]] DraftValue*
edit(PacketDraft& draft, std::uint16_t fieldIndex, std::uint32_t element, ValueRole role) noexcept;

/** Finds or creates one value inside a selected runtime schema. */
[[nodiscard]] DraftValue* edit_selected(PacketDraft& draft,
                                        std::uint16_t fieldIndex,
                                        std::uint32_t element,
                                        std::uint32_t schemaHandle,
                                        std::uint32_t schemaRow,
                                        std::uint32_t fieldRow,
                                        std::uint32_t occurrence) noexcept;

} // namespace sunrise::middleware::bap::activity_message::wire_schema
