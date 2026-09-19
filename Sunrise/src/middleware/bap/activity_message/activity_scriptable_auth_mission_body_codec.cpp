#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

#include "scriptable_auth_internal.h"

// The fixed-width scriptable-auth bodies that drive mission progress and the HUD: sequences,
// cinematics, objective resets, tasks, directives, the encounter observer, the
// public-event sensor and the area watcher. Each family has one encoder and one checker.

namespace sunrise::middleware::bap::activity_message::scriptable_auth {
namespace {

namespace bits = encoding::bits;

/** Client no-timer sentinel for the 0x808099C4 epoch field; 0 means a live 0-second timer. */
constexpr std::uint64_t kNoTimerEpoch = 0xFFFFFFFFFFFFFFFFULL;
/** Type-5 reserves the largest revision as its inactive value, and its table is fixed width. */
constexpr std::uint8_t kSequenceInactiveRevision = 0xFFU;
constexpr std::size_t kSequenceReferenceCount = 128U;
/** Field widths of the type-3 objective reset body, in bits. */
constexpr std::uint8_t kType3ValueWidth = 7;
constexpr std::uint8_t kType3GenerationWidth = 31;
/** Field widths of the type-68 directive lanes, in bits. */
constexpr std::uint8_t kDirectiveStateWidth = 2;
constexpr std::uint8_t kDirectiveAuxStateWidth = 3;
constexpr std::uint8_t kDirectiveActiveIndexWidth = 3;
/** Marker modes the HUD eligibility pass reads from lane +100. */
constexpr std::uint32_t kTrackedMarkerMode = 0;
constexpr std::uint32_t kFlaggedMarkerMode = 1;
constexpr std::uint32_t kWaypointMarkerMode = 2;

/**
 * Writes the 353-bit 0x808099C4 child with no timer: epoch -1, all other fields 0.
 * Epoch 0
 * arms the client's goal-timer pin and the directive banner never hides.
 */
[[nodiscard]] bool write_neutral_timed_state(bits::Writer& writer) noexcept {
    if (!writer.write(0, kBoolWidth)) {
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        if (!writer.write(0, kWideIntegerWidth)) {
            return false;
        }
    }
    return writer.write(kNoTimerEpoch, kWideIntegerWidth) && writer.write(0, kReal32Width);
}

/** Writes an absent reference or an exact slot with the ClientRef type and index biases. */
[[nodiscard]] bool write_directive_reference(bits::Writer& writer,
                                             const Type2LaneClientRef& target) noexcept {
    return target.slotIndex < 0
               ? write_absent_client_ref(writer)
               : auth_fields::write_client_ref(writer,
                                               target.registryKey,
                                               static_cast<std::uint32_t>(target.slotType),
                                               static_cast<std::uint16_t>(target.slotIndex));
}

/** Writes one 239-bit authored HUD marker subrecord, targeting the given slot or none. */
[[nodiscard]] bool write_directive_marker(bits::Writer& writer,
                                          const Type2LaneClientRef& target,
                                          std::uint32_t nameHash = kClientRefAbsentKey,
                                          std::uint32_t bubbleHash = 0) noexcept {
    // The client copies .0 into its target when that resolves. The first hash of .2 is the
    // navpoint's name, which the client resolves to a position itself; the next word is the
    // navpoint's bubble, which decides whether the marker is a route to another bubble.
    return write_directive_reference(writer, target) && write_absent_client_ref(writer)
           && writer.write(nameHash, 32) && writer.write(bubbleHash, 32) && writer.write(0, 32)
           && writer.write(0, 32) && writer.write(0, kBoolWidth);
}

/** Writes one of the three complete type-68 state lanes. */
[[nodiscard]] bool write_directive_entry(bits::Writer& writer,
                                         std::uint32_t nameHash,
                                         std::int32_t elementIndex,
                                         std::int8_t state,
                                         const Type2LaneClientRef& target = {},
                                         std::uint32_t targetNameHash = kClientRefAbsentKey,
                                         std::uint32_t targetBubbleHash = 0,
                                         const Type2LaneClientRef& waypoint = {}) noexcept {
    if (!writer.write(nameHash, 32)
        || !writer.write(std::bit_cast<std::uint32_t>(elementIndex) + kSigned32Bias, 32)
        || !writer.write(static_cast<std::uint32_t>(state) + 1U, kDirectiveStateWidth)
        || !write_neutral_timed_state(writer)) {
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        if (!writer.write(kSigned32Bias, 32)) {
            return false;
        }
    }
    // Lane +92 is the player-set reference and +100 the marker mode the builder applies while the
    // player is in that set: 0 tracks, 1 needs the candidate flag, 2 never tracks, 3 builds none.
    const std::uint32_t markerMode = waypoint.slotIndex >= 0 ? kWaypointMarkerMode
                                     : target.slotIndex >= 0 ? kTrackedMarkerMode
                                                             : kFlaggedMarkerMode;
    if (!writer.write(1, kDirectiveStateWidth) || !write_directive_reference(writer, waypoint)
        || !writer.write(markerMode, kDirectiveAuxStateWidth)) {
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        const bool first = index == 0;
        if (!write_directive_marker(writer,
                                    first ? target : Type2LaneClientRef{},
                                    first ? targetNameHash : kClientRefAbsentKey,
                                    first ? targetBubbleHash : 0U)) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Finds the next sequence revision while reserving 0xFF as inactive. */
bool next_type5_revision(const Type5RevisionGuard& guard, std::uint8_t& next) noexcept {
    next = 1U;
    if (!guard.hasLast) {
        return true;
    }
    if (guard.last >= kSequenceInactiveRevision - 1U) {
        return false;
    }
    next = static_cast<std::uint8_t>(guard.last + 1U);
    return next != kSequenceInactiveRevision;
}

/** Encodes the canonical sequence body: one revision and 129 unset ClientRefs. */
bool encode_type5(const Type5Preset& preset,
                  const Type5RevisionGuard& guard,
                  std::span<std::byte> output,
                  std::size_t& written) noexcept {
    written = 0;
    if (preset.revision == kSequenceInactiveRevision
        || (guard.hasLast && preset.revision <= guard.last) || output.size() < kType5ByteCount) {
        return false;
    }
    bits::Writer writer(output.first(kType5ByteCount));
    bool encoded = writer.write(0, kWideIntegerWidth) && writer.write(0, kWideIntegerWidth)
                   && writer.write(preset.revision, 8U);
    for (std::size_t index = 0; encoded && index < 4U; ++index) {
        encoded = writer.write(0, kSigned32Width);
    }
    for (std::size_t index = 0; encoded && index < kSequenceReferenceCount; ++index) {
        encoded = write_absent_client_ref(writer);
    }
    encoded = encoded && write_absent_client_ref(writer);
    return encoded && writer.bit_count() == kType5BitCount && writer.finish(written)
           && written == kType5ByteCount;
}

/** Accepts only the canonical closed type-5 body emitted by encode_type5. */
bool validate_type5_body(std::span<const std::byte> input, std::size_t bitCount) noexcept {
    if (bitCount != kType5BitCount || input.size() != kType5ByteCount) {
        return false;
    }
    bits::Reader reader(input);
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint64_t revision = 0;
    if (!reader.read(kWideIntegerWidth, first) || !reader.read(kWideIntegerWidth, second)
        || !reader.read(8U, revision) || first != 0 || second != 0
        || revision == kSequenceInactiveRevision) {
        return false;
    }
    for (std::size_t index = 0; index < 4U; ++index) {
        std::uint64_t value = 0;
        if (!reader.read(kSigned32Width, value) || value != 0) {
            return false;
        }
    }
    for (std::size_t index = 0; index < kSequenceReferenceCount + 1U; ++index) {
        if (!read_absent_client_ref(reader)) {
            return false;
        }
    }
    return finish_padding(reader);
}

/** Finds the next cinematic generation without wrapping. */
bool next_type6_generation(const Type6GenerationGuard& guard, std::uint32_t& next) noexcept {
    next = 1U;
    if (!guard.hasLast) {
        return true;
    }
    if (guard.last == (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    next = guard.last + 1U;
    return next != 0;
}

/** Encodes the canonical cinematic start/stop body with empty participant arrays. */
bool encode_type6(const Type6Preset& preset,
                  const Type6GenerationGuard& guard,
                  std::span<std::byte> output,
                  std::size_t& written) noexcept {
    written = 0;
    if (preset.generation == 0 || (guard.hasLast && preset.generation <= guard.last)
        || output.size() < kType6ByteCount) {
        return false;
    }
    bits::Writer writer(output.first(kType6ByteCount));
    bool encoded = writer.write(0, kWideIntegerWidth) && writer.write(0, kWideIntegerWidth)
                   && writer.write(preset.generation, kSigned32Width)
                   && writer.write(preset.active ? 1U : 0U, kBoolWidth)
                   && writer.write(0, kBoolWidth)
                   && write_absent_client_ref(writer)
                   // Bias three stores the native wildcard value -1 as wire value two.
                   && writer.write(2U, 6U)
                   // Both arrays are dynamically sized. Zero counts have no element payload.
                   && writer.write(0, 5U) && writer.write(0, 3U) && writer.write(0, kSigned32Width);
    return encoded && writer.bit_count() == kType6BitCount && writer.finish(written)
           && written == kType6ByteCount;
}

/** Accepts only the canonical closed type-6 body emitted by encode_type6. */
bool validate_type6_body(std::span<const std::byte> input, std::size_t bitCount) noexcept {
    if (bitCount != kType6BitCount || input.size() != kType6ByteCount) {
        return false;
    }
    bits::Reader reader(input);
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint64_t generation = 0;
    std::uint64_t active = 0;
    std::uint64_t force = 0;
    std::uint64_t role = 0;
    std::uint64_t count = 0;
    if (!reader.read(kWideIntegerWidth, first) || !reader.read(kWideIntegerWidth, second)
        || !reader.read(kSigned32Width, generation) || !reader.read(kBoolWidth, active)
        || !reader.read(kBoolWidth, force) || !read_absent_client_ref(reader)
        || !reader.read(6U, role) || !reader.read(5U, count) || first != 0 || second != 0
        || generation == 0 || force != 0 || role != 2U || count != 0) {
        return false;
    }
    if (!reader.read(3U, count) || count != 0) {
        return false;
    }
    std::uint64_t trailing = 0;
    return reader.read(kSigned32Width, trailing) && trailing == 0 && finish_padding(reader);
}

/** Advances the exact signed type-3 generation without wrapping its terminal value. */
bool next_type3_generation(const Type3GenerationGuard& guard, std::int32_t& next) noexcept {
    return next_positive_generation(guard.hasLast, guard.last, next);
}

/** Encodes the fixed 24-objective reset body and its monotonic generation. */
bool encode_type3(const Type3Body& body,
                  const Type3GenerationGuard& guard,
                  std::span<std::byte> output,
                  std::size_t& written) noexcept {
    written = 0;
    if (body.generation <= 0 || (guard.hasLast && body.generation <= guard.last)
        || output.size() < kType3ByteCount) {
        return false;
    }
    bits::Writer writer(output.first(kType3ByteCount));
    bool encoded = writer.write(1U, kBoolWidth);
    for (const std::int8_t value : body.objectiveRevisions) {
        if (value < 0) {
            return false;
        }
        encoded = encoded && writer.write(1U, kBoolWidth)
                  && writer.write(static_cast<std::uint8_t>(value), kType3ValueWidth);
    }
    encoded = encoded && writer.write(1U, kBoolWidth)
              && writer.write(static_cast<std::uint32_t>(body.generation), kType3GenerationWidth);
    return encoded && writer.bit_count() == kType3BitCount && writer.finish(written)
           && written == kType3ByteCount;
}

/** Decodes one complete fixed-width type-3 objective reset body. */
bool decode_type3_body(std::span<const std::byte> input, Type3Body& body) noexcept {
    body = {};
    if (input.size() != kType3ByteCount) {
        return false;
    }
    bits::Reader reader(input);
    std::uint64_t arrayPresent = 0;
    if (!reader.read(kBoolWidth, arrayPresent) || arrayPresent == 0) {
        return false;
    }
    for (std::int8_t& value : body.objectiveRevisions) {
        std::uint64_t present = 0;
        std::uint64_t wire = 0;
        if (!reader.read(kBoolWidth, present)
            || (present != 0 && !reader.read(kType3ValueWidth, wire))) {
            return false;
        }
        value = present != 0 ? static_cast<std::int8_t>(wire) : std::int8_t{0};
    }
    std::uint64_t present = 0;
    std::uint64_t generation = 0;
    if (!reader.read(kBoolWidth, present)
        || (present != 0 && !reader.read(kType3GenerationWidth, generation))
        || !finish_padding(reader)) {
        return false;
    }
    body.generation = present != 0 ? static_cast<std::int32_t>(generation) : 0;
    return body.generation > 0;
}

/** Validates the exact type-3 bit count and canonical fixed-width body. */
bool validate_type3_body(std::span<const std::byte> input, std::size_t bitCount) noexcept {
    Type3Body body{};
    return bitCount == kType3BitCount && decode_type3_body(input, body);
}

/** Finds the next positive task generation without wrapping. */
bool next_type38_generation(const Type38GenerationGuard& guard, std::int32_t& next) noexcept {
    return next_positive_generation(guard.hasLast, guard.last, next);
}

/** Encodes one exact signed-int32 task generation. */
bool encode_type38(const Type38Preset& preset,
                   const Type38GenerationGuard& guard,
                   std::span<std::byte> output,
                   std::size_t& written) noexcept {
    written = 0;
    if (preset.generation <= 0 || (guard.hasLast && preset.generation <= guard.last)
        || output.size() < kType38ByteCount) {
        return false;
    }
    bits::Writer writer(output.first(kType38ByteCount));
    const std::uint32_t wire = std::bit_cast<std::uint32_t>(preset.generation) + kSigned32Bias;
    return writer.write(wire, kSigned32Width) && writer.bit_count() == kType38BitCount
           && writer.finish(written) && written == kType38ByteCount;
}

/** Decodes one exact signed-int32 task generation. */
bool decode_type38_body(std::span<const std::byte> input, std::int32_t& generation) noexcept {
    if (input.size() != kType38ByteCount) {
        return false;
    }
    bits::Reader reader(input);
    std::uint64_t wire = 0;
    if (!reader.read(kSigned32Width, wire) || !finish_padding(reader)) {
        return false;
    }
    generation = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(wire) - kSigned32Bias);
    return generation > 0;
}

/** Checks one packed type-38 task generation. */
bool validate_type38_body(std::span<const std::byte> input, std::size_t bitCount) noexcept {
    std::int32_t generation = 0;
    return bitCount == kType38BitCount && decode_type38_body(input, generation);
}

/** Encodes one complete three-lane type-68 directive state. */
bool encode_type68(const Type68Preset& preset,
                   std::span<std::byte> output,
                   std::size_t& written) noexcept {
    written = 0;
    const auto valid_reference = [](const Type2LaneClientRef& reference, std::int8_t slotType) {
        return reference.slotIndex < 0
               || (reference.slotType == slotType && reference.registryKey != 0
                   && reference.registryKey != kClientRefAbsentKey);
    };
    // A navpoint rides only on a visible entry in the enter state.
    const bool navpointAllowed =
        preset.navpoint.slotIndex < 0 || (preset.visible && preset.state == 0);
    if (!valid_reference(preset.audience, kType70SlotType)
        || !valid_reference(preset.navpoint, kType47SlotType) || !navpointAllowed
        || !valid_reference(preset.waypoint, kType60SlotType)
        || (preset.waypoint.slotIndex >= 0 && !preset.visible) || output.size() < kType68ByteCount
        || preset.state < 0 || preset.state > 2
        || (preset.visible
            && (preset.nameHash == 0 || preset.nameHash == kClientRefAbsentKey
                || preset.elementIndex < 0))) {
        return false;
    }
    bits::Writer writer(output.first(kType68ByteCount));
    bool encoded =
        write_directive_reference(writer, preset.audience) && write_absent_client_ref(writer);
    for (std::size_t index = 0; encoded && index < kType68EntryCount; ++index) {
        encoded = index == 0 && preset.visible
                      ? write_directive_entry(writer,
                                              preset.nameHash,
                                              preset.elementIndex,
                                              preset.state,
                                              preset.navpoint,
                                              preset.navpointNameHash,
                                              preset.navpointBubbleHash,
                                              preset.waypoint)
                      : write_directive_entry(writer, kClientRefAbsentKey, 0, -1);
    }
    encoded = encoded && writer.write(preset.visible ? 1U : 0U, kDirectiveActiveIndexWidth);
    return encoded && writer.bit_count() == kType68BitCount && writer.finish(written)
           && written == kType68ByteCount;
}

/** Validates the crash-bearing selector fields and the exact type-68 body shape. */
bool validate_type68_body(std::span<const std::byte> input, std::size_t bitCount) noexcept {
    if (bitCount != kType68BitCount || input.size() != kType68ByteCount) {
        return false;
    }
    bits::Reader reader(input);
    auto skip = [&reader](std::size_t count) noexcept {
        std::uint64_t discarded = 0;
        while (count != 0) {
            const auto width = static_cast<std::uint8_t>((std::min)(count, std::size_t{64}));
            if (!reader.read(width, discarded)) {
                return false;
            }
            count -= width;
        }
        return true;
    };
    if (!skip(110)) {
        return false;
    }
    std::array<std::uint32_t, kType68EntryCount> names{};
    std::array<std::int32_t, kType68EntryCount> elements{};
    std::array<std::int32_t, kType68EntryCount> states{};
    for (std::size_t index = 0; index < kType68EntryCount; ++index) {
        std::uint64_t name = 0;
        std::uint64_t element = 0;
        std::uint64_t state = 0;
        if (!reader.read(32, name) || !reader.read(32, element)
            || !reader.read(kDirectiveStateWidth, state) || !skip(1'497)) {
            return false;
        }
        names[index] = static_cast<std::uint32_t>(name);
        elements[index] =
            std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(element) - kSigned32Bias);
        states[index] = static_cast<std::int32_t>(state) - 1;
    }
    std::uint64_t activeWire = 0;
    if (!reader.read(kDirectiveActiveIndexWidth, activeWire)) {
        return false;
    }
    const std::int32_t active = static_cast<std::int32_t>(activeWire) - 1;
    if (active < -1 || active >= static_cast<std::int32_t>(kType68EntryCount)) {
        return false;
    }
    if (active >= 0) {
        const std::size_t selected = static_cast<std::size_t>(active);
        if (names[selected] == 0 || names[selected] == kClientRefAbsentKey || elements[selected] < 0
            || states[selected] < -1 || states[selected] > 2) {
            return false;
        }
    }
    return finish_padding(reader);
}

/** Encodes the no-filter type-70 encounter engagement state. */
bool encode_type70(const Type70Preset& preset,
                   std::span<std::byte> output,
                   std::size_t& written) noexcept {
    written = 0;
    if (preset.flags > 0x1FU || output.size() < kType70ByteCount) {
        return false;
    }
    bits::Writer writer(output.first(kType70ByteCount));
    const auto revision = static_cast<std::uint16_t>(static_cast<std::int32_t>(preset.revision)
                                                     + static_cast<std::int32_t>(kSigned16Bias));
    const bool encoded = writer.write(preset.flags, 5U)
                         // Both dynamic collections are absent; no element payload follows.
                         && writer.write(0, kBoolWidth) && writer.write(revision, 16U)
                         && writer.write(0, kBoolWidth);
    return encoded && writer.bit_count() == kType70BitCount && writer.finish(written)
           && written == kType70ByteCount;
}

/** Validates the exact no-filter type-70 form while preserving all five flag bits. */
bool validate_type70_body(std::span<const std::byte> input, std::size_t bitCount) noexcept {
    if (bitCount != kType70BitCount || input.size() != kType70ByteCount) {
        return false;
    }
    bits::Reader reader(input);
    std::uint64_t flags = 0;
    std::uint64_t playerListPresent = 0;
    std::uint64_t revision = 0;
    std::uint64_t definitionListPresent = 0;
    return reader.read(5U, flags) && flags <= 0x1FU && reader.read(kBoolWidth, playerListPresent)
           && playerListPresent == 0 && reader.read(16U, revision)
           && reader.read(kBoolWidth, definitionListPresent) && definitionListPresent == 0
           && finish_padding(reader);
}

bool next_type42_generation(const Type42GenerationGuard& guard, std::int32_t& next) noexcept {
    return next_positive_generation(guard.hasLast, guard.last, next);
}

/** The client treats the FNV-1 basis as "no performance", so neither it nor zero may be sent. */
[[nodiscard]] bool valid_type42_name(std::uint32_t nameHash) noexcept {
    return nameHash != 0 && nameHash != kClientRefAbsentKey;
}

/** Refuses a generation that does not rise, because the client starts the state only once. */
bool encode_type42(const Type42Preset& preset,
                   const Type42GenerationGuard& guard,
                   std::span<std::byte> output,
                   std::size_t& written) noexcept {
    written = 0;
    if (!valid_type42_name(preset.nameHash) || preset.generation <= 0
        || (guard.hasLast && preset.generation <= guard.last) || output.size() < kType42ByteCount) {
        return false;
    }
    bits::Writer writer(output.first(kType42ByteCount));
    const std::uint32_t generation =
        std::bit_cast<std::uint32_t>(preset.generation) + kSigned32Bias;
    const bool encoded = writer.write(kEnabled, kBoolWidth) && writer.write(preset.nameHash, 32U)
                         && writer.write(preset.value, 32U)
                         && writer.write(generation, kSigned32Width)
                         // The row table is absent; the client keeps its own rows.
                         && writer.write(0, kBoolWidth);
    return encoded && writer.bit_count() == kType42BitCount && writer.finish(written)
           && written == kType42ByteCount;
}

/** Accepts only the command group present, a real state name, and an absent row table. */
bool validate_type42_body(std::span<const std::byte> input, std::size_t bitCount) noexcept {
    if (bitCount != kType42BitCount || input.size() != kType42ByteCount) {
        return false;
    }
    bits::Reader reader(input);
    std::uint64_t commandPresent = 0;
    std::uint64_t nameHash = 0;
    std::uint64_t value = 0;
    std::uint64_t generation = 0;
    std::uint64_t rowsPresent = 0;
    return reader.read(kBoolWidth, commandPresent) && commandPresent == kEnabled
           && reader.read(32U, nameHash) && valid_type42_name(static_cast<std::uint32_t>(nameHash))
           && reader.read(32U, value) && reader.read(kSigned32Width, generation)
           && generation > kSigned32Bias && reader.read(kBoolWidth, rowsPresent) && rowsPresent == 0
           && finish_padding(reader);
}

/** @return True when the area reference names a real slot the client evaluator can resolve. */
[[nodiscard]] bool valid_type71_area(const Type71Body& body) noexcept {
    return body.areaRegistryKey != 0 && body.areaRegistryKey != kClientRefAbsentKey
           && body.areaSlotType < (std::uint8_t{1} << kClientRefTypeWidth) - 1U
           && body.areaSlotIndex < kClientRefIndexBias;
}

/** Encodes the fixed 183-bit type-71 body. */
bool encode_type71(const Type71Body& body,
                   std::span<std::byte> output,
                   std::size_t& written) noexcept {
    written = 0;
    if (!valid_type71_area(body) || !std::isfinite(body.leaveSeconds) || body.leaveSeconds < 0.0F
        || output.size() < kType71ByteCount) {
        return false;
    }
    bits::Writer writer(output.first(kType71ByteCount));
    const std::uint32_t state = std::bit_cast<std::uint32_t>(body.state) + kSigned32Bias;
    const bool encoded =
        writer.write(state, kSigned32Width) && writer.write(body.playerIdentity, kWideIntegerWidth)
        && writer.write(body.areaRegistryKey, 32)
        && writer.write(std::uint32_t{body.areaSlotType} + 1U, kClientRefTypeWidth)
        && writer.write(std::uint32_t{body.areaSlotIndex} + kClientRefIndexBias,
                        kClientRefIndexWidth)
        && writer.write(std::bit_cast<std::uint32_t>(body.leaveSeconds), kReal32Width);
    return encoded && writer.bit_count() == kType71BitCount && writer.finish(written)
           && written == kType71ByteCount;
}

/** Decodes one complete type-71 body. */
bool decode_type71_body(std::span<const std::byte> input, Type71Body& body) noexcept {
    body = {};
    if (input.size() != kType71ByteCount) {
        return false;
    }
    bits::Reader reader(input);
    std::uint64_t state = 0;
    std::uint64_t identity = 0;
    std::uint64_t key = 0;
    std::uint64_t type = 0;
    std::uint64_t index = 0;
    std::uint64_t seconds = 0;
    if (!reader.read(kSigned32Width, state) || !reader.read(kWideIntegerWidth, identity)
        || !reader.read(32, key) || !reader.read(kClientRefTypeWidth, type)
        || !reader.read(kClientRefIndexWidth, index) || !reader.read(kReal32Width, seconds)
        || !finish_padding(reader) || type == 0 || index < kClientRefIndexBias) {
        return false;
    }
    Type71Body parsed{};
    parsed.state = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(state) - kSigned32Bias);
    parsed.playerIdentity = identity;
    parsed.areaRegistryKey = static_cast<std::uint32_t>(key);
    parsed.areaSlotType = static_cast<std::uint8_t>(type - 1U);
    parsed.areaSlotIndex = static_cast<std::uint16_t>(index - kClientRefIndexBias);
    parsed.leaveSeconds = std::bit_cast<float>(static_cast<std::uint32_t>(seconds));
    if (!valid_type71_area(parsed) || !std::isfinite(parsed.leaveSeconds)
        || parsed.leaveSeconds < 0.0F) {
        return false;
    }
    body = parsed;
    return true;
}

/** Validates the exact type-71 body shape and its present area reference. */
bool validate_type71_body(std::span<const std::byte> input, std::size_t bitCount) noexcept {
    Type71Body body{};
    return bitCount == kType71BitCount && decode_type71_body(input, body);
}

} // namespace sunrise::middleware::bap::activity_message::scriptable_auth
