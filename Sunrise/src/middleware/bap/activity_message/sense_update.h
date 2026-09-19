#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "activity_patch_epoch_parser.h"

namespace sunrise::middleware::bap::activity_message::sense_update {

/** The client reports sensor sense changes. It is the client's answer to the roster update. */
inline constexpr std::uint32_t kMessageType = 6;

/** The body opens with the same 128-bit patch epoch the roster update echoes. */
inline constexpr std::uint8_t kEpochFieldWidth = 64;
/** One literal zero bit follows the epoch. A set bit means the body is not this shape. */
inline constexpr std::uint8_t kLiteralZeroWidth = 1;
/** The client's outer destination bounds the whole body. This native capacity is in bytes. */
inline constexpr std::size_t kOuterByteCapacity = 514'048;
/** The client's per-group scratch bounds one group substream. This capacity is also in bytes. */
inline constexpr std::size_t kGroupByteCapacity = 102'400;
/** Owned object rows per packet, shared by diagnostics and accepted mission input.
 * Build-86657 region transitions exceed 128 rows. Overflow remains explicit and
 * incomplete values are never delivered; this is a storage budget, not a wire limit.
 */
inline constexpr std::size_t kDecodedObjectCapacity = 256;
/** Owned scalar rows, including absent fields, in the bounded packet storage budget. */
inline constexpr std::size_t kDecodedValueCapacity = 4096;
/** Runtime SDK row sentinel shared without depending on the State layer. */
inline constexpr std::uint32_t kAbsentRuntimeRow = (std::numeric_limits<std::uint32_t>::max)();

/** How completely one msg-6 body was decoded. */
enum class DecodeStatus : std::uint8_t {
    complete,
    /** The framing closed, but at least one declared group could only be skipped. */
    partial,
    /** The msg-6 root schema was unavailable, so the group loop could not be located. */
    schemaUnavailable,
    malformed,
};

/** Result of resolving one committed group or one exact ClientRef slot. */
enum class TargetStatus : std::uint8_t {
    resolved,
    targetUnavailable,
    schemaUnavailable,
};

/** Per-object result retained even when the enclosing group must be skipped. */
enum class ObjectStatus : std::uint8_t {
    decoded,
    targetUnavailable,
    schemaUnavailable,
    unsupportedField,
    unsafeCount,
    malformed,
};

/** Scalar domains emitted by the authored native Sense codecs. */
enum class ValueKind : std::uint8_t {
    unsignedInteger,
    signedInteger,
    boolean,
    real32,
};

/** Exact committed roster object selected before any group substream is interpreted. */
struct GroupTarget final {
    std::uint32_t objectTag{};
    std::uint32_t objectRow{kAbsentRuntimeRow};
};

/** Exact SDK slot and Sense root selected from one ClientRef. */
struct SlotTarget final {
    std::uint32_t slotRow{kAbsentRuntimeRow};
    std::uint32_t senseSchema{};
    std::uint32_t schemaRow{kAbsentRuntimeRow};
};

using ResolveGroup = TargetStatus (*)(const void* context,
                                      std::uint32_t registryKey,
                                      GroupTarget& output) noexcept;
using ResolveSlot = TargetStatus (*)(const void* context,
                                     const GroupTarget& group,
                                     std::uint8_t slotType,
                                     std::uint16_t slotIndex,
                                     SlotTarget& output) noexcept;
/** Borrowed adapters used only to bind a wire ClientRef to its native slot declaration. */
struct Resolver final {
    const void* context{};
    ResolveGroup resolveGroup{};
    ResolveSlot resolveSlot{};
};

/** One value-owned scalar. No span or raw payload survives the route call. */
struct DecodedValue final {
    std::uint64_t unsignedValue{};
    std::int64_t signedValue{};
    float realValue{};
    std::uint32_t schemaRow{kAbsentRuntimeRow};
    std::uint32_t fieldRow{kAbsentRuntimeRow};
    std::uint32_t occurrence{};
    std::uint32_t bitOffset{};
    std::uint16_t fieldOrdinal{};
    std::uint8_t width{};
    ValueKind kind{ValueKind::unsignedInteger};
    bool present{true};
};

/** One ClientRef and the contiguous retained values decoded from its selected Sense root. */
struct DecodedObject final {
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint32_t objectRow{kAbsentRuntimeRow};
    std::uint32_t slotRow{kAbsentRuntimeRow};
    std::uint32_t senseSchema{};
    std::uint32_t schemaRow{kAbsentRuntimeRow};
    std::uint32_t generationPlusOne{};
    std::uint32_t firstValue{};
    std::uint32_t valueCount{};
    std::uint32_t deltaBits{};
    std::uint16_t slotIndex{};
    std::uint8_t slotType{};
    ObjectStatus status{ObjectStatus::malformed};
    bool hasGeneration{};
};

/** Bounded typed result used by the Activity Host packet-detail ring. */
struct DecodedPacket final {
    std::array<DecodedObject, kDecodedObjectCapacity> objects{};
    std::array<DecodedValue, kDecodedValueCapacity> values{};
    std::size_t objectCount{};
    std::size_t valueCount{};
    std::size_t bitsConsumed{};
    std::size_t bitsRemaining{};
    std::uint32_t groupsSeen{};
    std::uint32_t groupsDecoded{};
    std::uint32_t groupsSkipped{};
    std::uint32_t objectsSeen{};
    std::uint32_t objectsDecoded{};
    std::uint8_t paddingBits{};
    DecodeStatus status{DecodeStatus::malformed};
    bool objectsTruncated{};
    bool valuesTruncated{};
};

/** A sensor sense update: the prefix facts always, and a typed decode when a schema resolved. */
struct SenseUpdate {
    /** Epoch the client believes is current. It must match the one the roster update carried. */
    patch_epoch::PatchEpoch epoch{};
    /** Bits left after the literal zero. The prefix diagnostic reports this. */
    std::uint32_t tailBits{};
    /** Declared width of the first emitted group, when its root and header were readable. */
    std::uint32_t firstGroupBits{};
    /** Registry key of the first emitted group and first ClientRef. */
    std::uint32_t firstRegistryKey{};
    /** First ClientRef's unbiased package slot index. */
    std::uint16_t firstSlotIndex{};
    /** First ClientRef's unbiased package slot type. */
    std::uint8_t firstSlotType{};
    /** True only when the exact root, first group header, and first ClientRef all closed. */
    bool hasFirstObject{};
    /** Complete typed result when every selected slot has an authored native decoder. */
    DecodedPacket decoded{};
};

/**
 * Parses a sensor sense update as far as its known grammar reaches.
 * @param input Activity payload after the envelope.
 * @param update Cleared first, then filled with the epoch and the tail size.
 * @param consumedBits Receives the bits the known prefix used.
 * @return True when the epoch and the literal zero were both present and the zero read zero.
 */
[[nodiscard]] bool parse_sense_update(std::span<const std::byte> input,
                                      SenseUpdate& update,
                                      std::size_t& consumedBits) noexcept;

/**
 * Decodes the complete slot-selected msg-6 grammar without retaining input storage.
 * Every supported slot layout is an authored native decoder; no runtime schema walk occurs.
 */
[[nodiscard]] bool decode_sense_update(std::span<const std::byte> input,
                                       const Resolver& resolver,
                                       SenseUpdate& update,
                                       std::size_t& consumedBits) noexcept;

[[nodiscard]] const char* decode_status_name(DecodeStatus status) noexcept;
[[nodiscard]] const char* object_status_name(ObjectStatus status) noexcept;

} // namespace sunrise::middleware::bap::activity_message::sense_update
