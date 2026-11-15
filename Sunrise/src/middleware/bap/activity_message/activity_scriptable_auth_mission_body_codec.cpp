#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>

#include "darkness_zone_auth.h"
#include "scriptable_auth_internal.h"

// The fixed-width scriptable-auth bodies that drive mission progress and the HUD: sequences,
// cinematics, objective resets, tasks, directives, the encounter observer, the
// public-event sensor and the area watcher. Each family has an encoder, and a decoder where the
// host reads the body back.

namespace sunrise::middleware::bap::activity_message::scriptable_auth {
namespace {

namespace bits = encoding::bits;

/** Client no-timer sentinel for the timed-state epoch; 0 means a live 0-second timer. */
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
/** The type-70 root preserves five native flags, independently of its optional lists. */
constexpr std::uint8_t kEngagementFlagWidth = 5;
constexpr std::uint8_t kEngagementFlagMask = (1U << kEngagementFlagWidth) - 1U;
/** The type-70 revision is a signed 16-bit schema field. */
constexpr std::uint8_t kEngagementRevisionWidth = 16;
static_assert(kType70MaximumByteCount
              == (kType70BitCount + kType70VolumeCountWidth
                  + kType70VolumeCapacity * auth_fields::kClientRefBits + 7U)
                     / 8U);

/**
 * Writes the 353-bit timed-state child with no timer: epoch -1, all other fields 0.
 * Epoch 0 arms the client's goal-timer pin and the directive banner never hides.
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

/** Writes one 239-bit authored HUD marker subrecord, targeting the given slot or none. */
[[nodiscard]] bool write_directive_marker(bits::Writer& writer,
                                          const Type2LaneClientRef& target,
                                          std::uint32_t nameHash = kClientRefAbsentKey,
                                          std::uint32_t bubbleHash = 0) noexcept {
    // The client copies .0 into its target when that resolves. The first hash of .2 is the
    // navpoint's name, which the client resolves to a position itself; the next word is the
    // navpoint's bubble, which decides whether the marker is a route to another bubble.
    return write_optional_client_ref(writer, target) && write_absent_client_ref(writer)
           && writer.write(nameHash, 32) && writer.write(bubbleHash, 32) && writer.write(0, 32)
           && writer.write(0, 32) && writer.write(0, kBoolWidth);
}

/** Writes one of the three complete type-68 state lanes; a null clock writes the no-timer child. */
[[nodiscard]] bool
write_directive_entry(bits::Writer& writer,
                      std::uint32_t nameHash,
                      std::int32_t elementIndex,
                      std::int8_t state,
                      const Type2LaneClientRef& target = {},
                      std::uint32_t targetNameHash = kClientRefAbsentKey,
                      std::uint32_t targetBubbleHash = 0,
                      const Type2LaneClientRef& waypoint = {},
                      std::span<const std::int32_t, kType68ProgressCount> progress =
                          std::array<std::int32_t, kType68ProgressCount>{},
                      const SharedTimedState* clock = nullptr,
                      std::int8_t phase = 0) noexcept {
    if (!writer.write(nameHash, 32)
        || !writer.write(std::bit_cast<std::uint32_t>(elementIndex) + kSigned32Bias, 32)
        || !writer.write(static_cast<std::uint32_t>(state) + 1U, kDirectiveStateWidth)
        || !(clock != nullptr ? write_shared_timed_state(writer, *clock)
                              : write_neutral_timed_state(writer))) {
        return false;
    }
    for (const std::int32_t value : progress) {
        if (!writer.write(std::bit_cast<std::uint32_t>(value) + kSigned32Bias, 32)) {
            return false;
        }
    }
    // Lane +88 is the presentation phase (-1 presented, 0 incoming, 1 active), +92 the player-set
    // reference and +100 the marker mode the builder applies while the player is in that set:
    // 0 tracks, 1 needs the candidate flag, 2 never tracks, 3 builds none.
    const std::uint32_t markerMode = waypoint.slotIndex >= 0 ? kWaypointMarkerMode
                                     : target.slotIndex >= 0 ? kTrackedMarkerMode
                                                             : kFlaggedMarkerMode;
    if (phase < -1 || phase > 1
        || !writer.write(static_cast<std::uint32_t>(phase + 1), kDirectiveStateWidth)
        || !write_optional_client_ref(writer, waypoint)
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

/** Encodes the sequence body: one revision, the choice mask and 129 unset ClientRefs. */
bool encode_type5(const Type5Preset& preset,
                  const Type5RevisionGuard& guard,
                  std::span<std::byte> output,
                  std::size_t& written) noexcept {
    written = 0;
    // A retained body repeats the committed revision; any other must be newer.
    const bool revisionValid = preset.selection.retain
                                   ? guard.hasLast && preset.revision == guard.last
                                   : !guard.hasLast || preset.revision > guard.last;
    if (preset.revision == kSequenceInactiveRevision || !revisionValid
        || output.size() < kType5ByteCount) {
        return false;
    }
    bits::Writer writer(output.first(kType5ByteCount));
    bool encoded = writer.write(0, kWideIntegerWidth) && writer.write(0, kWideIntegerWidth)
                   && writer.write(preset.revision, 8U);
    for (const std::uint32_t lane : preset.selection.choices) {
        encoded = encoded && writer.write(lane, kSigned32Width);
    }
    for (std::size_t index = 0; encoded && index < kSequenceReferenceCount; ++index) {
        encoded = write_absent_client_ref(writer);
    }
    encoded = encoded && write_absent_client_ref(writer);
    return encoded && writer.bit_count() == kType5BitCount && writer.finish(written)
           && written == kType5ByteCount;
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
    // A clock rides only on the visible entry, counting whole seconds down from its total.
    const bool clocked = preset.clockRemainingSeconds != kType68NoClock
                         || preset.clockTotalSeconds != kType68NoClock;
    const bool clockAllowed = !clocked
                              || (preset.visible && preset.clockRemainingSeconds >= 0
                                  && preset.clockTotalSeconds >= preset.clockRemainingSeconds
                                  && preset.clockTotalSeconds <= kType68MaximumClockSeconds);
    if (!valid_reference(preset.audience, kType70SlotType)
        || !valid_reference(preset.event, kType71SlotType)
        || !valid_reference(preset.navpoint, kType47SlotType) || !navpointAllowed || !clockAllowed
        || !valid_reference(preset.waypoint, kType60SlotType)
        || (preset.waypoint.slotIndex >= 0 && !preset.visible) || output.size() < kType68ByteCount
        || preset.state < 0 || preset.state > 2
        || (preset.visible
            && (preset.nameHash == 0 || preset.nameHash == kClientRefAbsentKey
                || preset.elementIndex < 0))) {
        return false;
    }
    // Equal clamps pin the displayed value; the window must span the authored duration.
    const std::uint64_t elapsedTicks =
        clocked
            ? static_cast<std::uint64_t>(preset.clockTotalSeconds - preset.clockRemainingSeconds)
                  * darkness_zone::kTicksPerSecond
            : 0;
    const std::uint64_t remainingTicks =
        clocked ? static_cast<std::uint64_t>(preset.clockRemainingSeconds)
                      * darkness_zone::kTicksPerSecond
                : 0;
    const SharedTimedState clock{.running = true,
                                 .minimum = 0,
                                 .maximum =
                                     clocked ? static_cast<std::uint64_t>(preset.clockTotalSeconds)
                                                   * darkness_zone::kTicksPerSecond
                                             : 0,
                                 .currentAtEpoch = elapsedTicks,
                                 .remainingAtEpoch = remainingTicks,
                                 .epoch = preset.clockEpochTicks,
                                 .rate = 1.0F};
    bits::Writer writer(output.first(kType68ByteCount));
    // The two leading references: the engagement sensor that gates the banner, then the
    // public-event sensor whose state the HUD record copies.
    bool encoded = write_optional_client_ref(writer, preset.audience)
                   && write_optional_client_ref(writer, preset.event);
    for (std::size_t index = 0; encoded && index < kType68EntryCount; ++index) {
        encoded = index == 0 && preset.visible
                      ? write_directive_entry(writer,
                                              preset.nameHash,
                                              preset.elementIndex,
                                              preset.state,
                                              preset.navpoint,
                                              preset.navpointNameHash,
                                              preset.navpointBubbleHash,
                                              preset.waypoint,
                                              preset.progress,
                                              clocked ? &clock : nullptr,
                                              preset.phase)
                      : write_directive_entry(writer, kClientRefAbsentKey, 0, -1);
    }
    encoded = encoded && writer.write(preset.visible ? 1U : 0U, kDirectiveActiveIndexWidth);
    return encoded && writer.bit_count() == kType68BitCount && writer.finish(written)
           && written == kType68ByteCount;
}

/** Encodes the type-70 engagement state with optional volume references. */
bool encode_type70(const Type70Preset& preset,
                   std::span<std::byte> output,
                   std::size_t& written,
                   std::size_t& bitCount) noexcept {
    written = 0;
    bitCount = 0;
    if (preset.flags > kEngagementFlagMask || preset.volumeCount > preset.volumes.size()) {
        return false;
    }
    for (std::size_t index = 0; index < preset.volumeCount; ++index) {
        const Type2LaneClientRef& volume = preset.volumes[index];
        if (volume.registryKey == 0 || volume.registryKey == kClientRefAbsentKey
            || volume.slotType != kType60SlotType || volume.slotIndex < 0) {
            return false;
        }
    }
    const std::size_t expectedBits =
        kType70BitCount
        + (preset.volumeCount == 0
               ? 0U
               : kType70VolumeCountWidth + preset.volumeCount * auth_fields::kClientRefBits);
    const std::size_t expectedBytes = (expectedBits + 7U) / 8U;
    if (output.size() < expectedBytes) {
        return false;
    }
    bits::Writer writer(output.first(expectedBytes));
    if (!writer.write(preset.flags, kEngagementFlagWidth)
        || !writer.write(preset.volumeCount != 0, kBoolWidth)
        || (preset.volumeCount != 0
            && !writer.write(preset.volumeCount, kType70VolumeCountWidth))) {
        return false;
    }
    for (std::size_t index = 0; index < preset.volumeCount; ++index) {
        if (!write_optional_client_ref(writer, preset.volumes[index])) {
            return false;
        }
    }
    const std::uint16_t revision = static_cast<std::uint16_t>(
        static_cast<std::int32_t>(preset.revision) + static_cast<std::int32_t>(kSigned16Bias));
    if (!writer.write(revision, kEngagementRevisionWidth) || !writer.write(0, kBoolWidth)
        || writer.bit_count() != expectedBits || !writer.finish(written)) {
        written = 0;
        return false;
    }
    bitCount = expectedBits;
    return true;
}

/** Decodes the bounded type-70 volume audience for SDK ownership validation. */
bool decode_type70(std::span<const std::byte> input,
                   std::size_t bitCount,
                   Type70Preset& output) noexcept {
    if (bitCount < kType70BitCount || input.size() > kType70MaximumByteCount
        || bitCount > input.size() * 8U || !auth_fields::bytes_match_bits(input.size(), bitCount)) {
        return false;
    }
    bits::Reader reader(input);
    Type70Preset decoded{};
    std::uint64_t flags = 0;
    std::uint64_t volumesPresent = 0;
    std::uint64_t count = 0;
    if (!reader.read(kEngagementFlagWidth, flags) || !reader.read(kBoolWidth, volumesPresent)
        || (volumesPresent != 0
            && (!reader.read(kType70VolumeCountWidth, count) || count == 0
                || count > decoded.volumes.size()))) {
        return false;
    }
    decoded.flags = static_cast<std::uint8_t>(flags);
    decoded.volumeCount = static_cast<std::uint8_t>(count);
    for (std::size_t index = 0; index < decoded.volumeCount; ++index) {
        std::uint64_t key = 0;
        std::uint64_t type = 0;
        std::uint64_t slot = 0;
        if (!reader.read(auth_fields::kClientRefKeyWidth, key)
            || !reader.read(kClientRefTypeWidth, type) || !reader.read(kClientRefIndexWidth, slot)
            || key == 0 || key == kClientRefAbsentKey || type != kType60SlotType + 1U
            || slot < kClientRefIndexBias) {
            return false;
        }
        decoded.volumes[index] = {static_cast<std::uint32_t>(key),
                                  static_cast<std::int8_t>(kType60SlotType),
                                  static_cast<std::int16_t>(slot - kClientRefIndexBias)};
    }
    std::uint64_t revision = 0;
    std::uint64_t playersPresent = 0;
    if (!reader.read(kEngagementRevisionWidth, revision) || !reader.read(kBoolWidth, playersPresent)
        || playersPresent != 0 || reader.remaining_bits() != input.size() * 8U - bitCount
        || !finish_padding(reader)) {
        return false;
    }
    decoded.revision = static_cast<std::int16_t>(static_cast<std::int32_t>(revision)
                                                 - static_cast<std::int32_t>(kSigned16Bias));
    output = decoded;
    return true;
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

/**
 * The client evaluator reads an area list from the spatial object these slot types resolve to.
 * Any other slot resolves to a component whose area offset is not a pointer, and the read faults.
 */
[[nodiscard]] constexpr bool spatial_area_slot_type(std::uint8_t slotType) noexcept {
    // Native slot ordinals whose descriptors carry the client evaluator's spatial area list.
    constexpr std::uint8_t kAreaType44 = 44, kAreaType45 = 45, kAreaType47 = 47, kAreaType48 = 48,
                           kAreaType59 = 59, kAreaType60 = 60;
    return slotType == kAreaType44 || slotType == kAreaType45 || slotType == kAreaType47
           || slotType == kAreaType48 || slotType == kAreaType59 || slotType == kAreaType60;
}

/** @return True when the area reference names a spatial slot the client evaluator can resolve. */
[[nodiscard]] bool valid_type71_area(const Type71Body& body) noexcept {
    return body.areaRegistryKey != 0 && body.areaRegistryKey != kClientRefAbsentKey
           && spatial_area_slot_type(body.areaSlotType) && body.areaSlotIndex < kClientRefIndexBias;
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

} // namespace sunrise::middleware::bap::activity_message::scriptable_auth
