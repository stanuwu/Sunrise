#include <array>
#include <bit>
#include <cmath>
#include <limits>

#include "scriptable_auth_internal.h"
#include "volume_toggle_auth.h"

// The fixed-width scriptable-auth bodies that drive device state: authored objects, device
// channels, configured-action pulses, triggers and timers. Each family has an encoder, and a
// decoder where the host reads the body back. Mission and HUD families live in the mission codec.

namespace sunrise::middleware::bap::activity_message::scriptable_auth {
namespace {

namespace bits = encoding::bits;

/** Width of the biased type-23 sequence field, in bits. */
constexpr std::uint8_t kSequenceWidth = 16;
/** The type-31 auxiliary field is reserved and always sent as zero. */
constexpr std::uint64_t kUnusedAuxiliary = 0;
/** Type-31 reserves the largest generation as its unset sentinel. */
constexpr std::uint64_t kReservedGeneration = std::numeric_limits<std::uint64_t>::max();
/** The type-4 selected-child count is two bits; the plate form carries exactly one child. */
constexpr std::uint8_t kType4ChildCountWidth = 2;
/** Raw zero in a biased revision is INT32_MIN, never newer than the plate's stored revisions. */
constexpr std::uint32_t kStaleRevision = 0;
/** The plate owner is a six-bit field at bias three; raw two is the neutral owner, -1. */
constexpr std::uint8_t kPlateOwnerWidth = 6;
constexpr std::uint32_t kPlateNeutralOwner = 2;

/**
 * Writes the plate child that matches the content defaults: label 0, stale inhibition and owner
 * revisions, no participant filter and both optional overrides null.
 */
[[nodiscard]] bool write_capture_plate_child(bits::Writer& writer) noexcept {
    return writer.write(1, kBoolWidth) && writer.write(kType4CapturePlateSchema, kSigned32Width)
           && writer.write(kSigned32Bias, kSigned32Width)
           && writer.write(kStaleRevision, kSigned32Width) && writer.write(0, kBoolWidth)
           && writer.write(kStaleRevision, kSigned32Width)
           && writer.write(kPlateNeutralOwner, kPlateOwnerWidth) && write_absent_client_ref(writer)
           && writer.write(0, kBoolWidth) && writer.write(0, kBoolWidth);
}

/**
 * Writes the object state-machine child: lanes 1-2 stale (raw 0, never newer), lane 3 the position
 * at the given sequence, with the snap revision advanced to the same value when snapping.
 */
[[nodiscard]] bool write_position_child(bits::Writer& writer,
                                        const Type4PositionChild& child) noexcept {
    const std::uint32_t sequence = std::bit_cast<std::uint32_t>(child.revision) + kSigned32Bias;
    bool written =
        writer.write(1, kBoolWidth) && writer.write(kType4PositionChildSchema, kSigned32Width);
    // Native lanes zero and one stay unchanged; lane two receives the position.
    constexpr int kUnchangedPositionLanes = 2;
    for (int lane = 0; lane < kUnchangedPositionLanes && written; ++lane) {
        written = writer.write(kStaleRevision, kSigned32Width)
                  && writer.write(kStaleRevision, kSigned32Width) && writer.write(0, kReal32Width);
    }
    return written && writer.write(sequence, kSigned32Width)
           && writer.write(child.snap ? sequence : kStaleRevision, kSigned32Width)
           && writer.write(std::bit_cast<std::uint32_t>(child.value), kReal32Width);
}

/** @return The reflected channel ordinal, or the channel count for an invalid enum value. */
[[nodiscard]] constexpr std::size_t channel_index(Type23Channel channel) noexcept {
    const auto index = static_cast<std::size_t>(channel);
    return index < kType23ChannelCount ? index : kType23ChannelCount;
}

/** @return True when one type-23 preset names a channel and advances its sequence. */
[[nodiscard]] bool valid_type23(const Type23Preset& preset,
                                const Type23SequenceGuard& guard) noexcept {
    const std::size_t index = channel_index(preset.channel);
    if (index == kType23ChannelCount || guard.last[index] < 0) {
        return false;
    }
    return preset.sequence > 0 && preset.sequence > guard.last[index];
}

/** Writes all three reflected type-23 channels. */
[[nodiscard]] bool write_type23(bits::Writer& writer, const Type23Body& body) noexcept {
    bool encoded = true;
    for (std::size_t index = 0; encoded && index < kType23ChannelCount; ++index) {
        const Type23ChannelState& channel = body.channels[index];
        const std::int32_t wireSequence =
            static_cast<std::int32_t>(channel.sequence) + static_cast<std::int32_t>(kSigned16Bias);
        encoded = writer.write(std::bit_cast<std::uint32_t>(channel.desiredValue), kReal32Width)
                  && writer.write(wireSequence, kSequenceWidth)
                  && writer.write(channel.snap ? 1U : 0U, kBoolWidth);
    }
    return encoded && writer.bit_count() == kType23BitCount;
}

/** Reads all three reflected type-23 channels; float values pass through unchanged. */
[[nodiscard]] bool read_type23(bits::Reader& reader, Type23Body& body) noexcept {
    Type23Body parsed{};
    for (Type23ChannelState& channel : parsed.channels) {
        std::uint64_t desired = 0;
        std::uint64_t sequence = 0;
        std::uint64_t snap = 0;
        if (!reader.read(kReal32Width, desired) || !reader.read(kSequenceWidth, sequence)
            || !reader.read(kBoolWidth, snap)) {
            return false;
        }
        channel.desiredValue = std::bit_cast<float>(static_cast<std::uint32_t>(desired));
        channel.sequence =
            static_cast<std::int16_t>(static_cast<std::int32_t>(sequence) - kSigned16Bias);
        channel.snap = snap != 0;
    }
    body = parsed;
    return true;
}

/** @return True when one type-31 generation is not older than the host's last one. */
[[nodiscard]] bool valid_type31(const Type31Preset& preset,
                                const Type31GenerationGuard& guard) noexcept {
    return preset.generation != kReservedGeneration
           && (!guard.hasLast || preset.generation >= guard.last);
}

/** Reads the complete fixed-width shared timed-state child. */
[[nodiscard]] bool read_shared_timed_state(bits::Reader& reader, SharedTimedState& state) noexcept {
    SharedTimedState parsed{};
    std::uint64_t flag = 0;
    if (!reader.read(kBoolWidth, flag)) {
        return false;
    }
    parsed.running = flag != 0;
    if (!reader.read(kWideIntegerWidth, parsed.minimum)
        || !reader.read(kWideIntegerWidth, parsed.maximum)
        || !reader.read(kWideIntegerWidth, parsed.currentAtEpoch)
        || !reader.read(kWideIntegerWidth, parsed.remainingAtEpoch)
        || !reader.read(kWideIntegerWidth, parsed.epoch)) {
        return false;
    }
    std::uint64_t real32Bits = 0;
    if (!reader.read(kReal32Width, real32Bits)) {
        return false;
    }
    parsed.rate = std::bit_cast<float>(static_cast<std::uint32_t>(real32Bits));
    state = parsed;
    return true;
}

} // namespace

/** Advances the authored-object generation without reaching its signed terminal value. */
bool next_type4_generation(const Type4GenerationGuard& guard, std::int32_t& next) noexcept {
    return next_positive_generation(guard.hasLast, guard.last, next);
}

/** Encodes one package-owned entry; the second flag sends its location, plus one optional child. */
bool encode_type4(const Type4Preset& preset,
                  const Type4GenerationGuard& guard,
                  std::span<std::byte> output,
                  std::size_t& written) noexcept {
    written = 0;
    const std::size_t bitCount = type4_bit_count(preset);
    const std::size_t byteCount = (bitCount + 7) / 8;
    // A position update repeats the committed generation; everything else advances it.
    const Type4PositionChild& position = preset.position;
    const bool generationAccepted = position.present
                                        ? guard.hasLast && preset.generation == guard.last
                                        : !guard.hasLast || preset.generation > guard.last;
    if (preset.generation <= 0 || preset.entryIndex < 0 || !generationAccepted
        || output.size() < byteCount
        || (position.present
            && (preset.capturePlate || position.revision <= 0
                || !(position.value >= 0.0F && position.value <= 1.0F)))) {
        return false;
    }
    bits::Writer writer(output.first(byteCount));
    const std::uint32_t generation =
        std::bit_cast<std::uint32_t>(preset.generation) + kSigned32Bias;
    const std::uint32_t entry = std::bit_cast<std::uint32_t>(preset.entryIndex) + kSigned32Bias;
    const std::uint32_t neutral = kSigned32Bias;
    const bool located = preset.location.slotIndex >= 0;
    const bool encoded =
        writer.write(generation, kSigned32Width) && writer.write(entry, kSigned32Width)
        && writer.write(preset.active ? 1U : 0U, kBoolWidth)
        && writer.write(located ? 1U : 0U, kBoolWidth) && writer.write(neutral, kSigned32Width)
        && write_optional_client_ref(writer, preset.location) && writer.write(0, kReal32Width)
        && writer.write(0, kReal32Width) && writer.write(0, kReal32Width)
        && writer.write(0, kBoolWidth)
        && writer.write(preset.capturePlate || position.present ? 1U : 0U, kType4ChildCountWidth)
        && (!preset.capturePlate || write_capture_plate_child(writer))
        && (!position.present || write_position_child(writer, position));
    return encoded && writer.bit_count() == bitCount && writer.finish(written)
           && written == byteCount;
}

/** Finds the next positive type-23 sequence without wrapping. */
bool next_type23_sequence(const Type23SequenceGuard& guard,
                          Type23Channel channel,
                          std::int16_t& next) noexcept {
    next = 0;
    const std::size_t index = channel_index(channel);
    if (index == kType23ChannelCount || guard.last[index] < 0
        || guard.last[index] == std::numeric_limits<std::int16_t>::max()) {
        return false;
    }
    next = static_cast<std::int16_t>(guard.last[index] + 1);
    return next > 0;
}

/** Encodes one canonical type-23 auth body. */
bool encode_type23(const Type23Preset& preset,
                   const Type23SequenceGuard& guard,
                   std::span<std::byte> output,
                   std::size_t& written) noexcept {
    written = 0;
    if (!valid_type23(preset, guard)) {
        return false;
    }
    Type23Body body{};
    const std::size_t selected = channel_index(preset.channel);
    body.channels[selected] = {preset.value, preset.sequence, preset.snap};
    return encode_type23_body(body, output, written);
}

/** Encodes one complete type-23 body. */
bool encode_type23_body(const Type23Body& body,
                        std::span<std::byte> output,
                        std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kType23ByteCount) {
        return false;
    }
    bits::Writer writer(output.first(kType23ByteCount));
    return write_type23(writer, body) && writer.finish(written) && written == kType23ByteCount;
}

/** Decodes one complete type-23 body. */
bool decode_type23_body(std::span<const std::byte> input, Type23Body& body) noexcept {
    if (input.size() != kType23ByteCount) {
        return false;
    }
    bits::Reader reader(input);
    Type23Body parsed{};
    if (!read_type23(reader, parsed) || !finish_padding(reader)) {
        return false;
    }
    body = parsed;
    return true;
}

/** Finds the next type-31 generation without reaching the reserved maximum value. */
bool next_type31_generation(const Type31GenerationGuard& guard, std::uint64_t& next) noexcept {
    next = 0;
    if (!guard.hasLast) {
        return true;
    }
    if (guard.last >= kReservedGeneration - 1) {
        return false;
    }
    next = guard.last + 1;
    return true;
}

/** A fresh trigger takes generation zero; every later arm or disarm takes the largest one. */
bool type31_arm_generation(const Type31GenerationGuard& guard, std::uint64_t& next) noexcept {
    next = guard.hasLast ? kReservedGeneration - 1 : 0;
    return true;
}

/** Encodes one canonical type-31 configured-action pulse. */
bool encode_type31(const Type31Preset& preset,
                   const Type31GenerationGuard& guard,
                   std::span<std::byte> output,
                   std::size_t& written) noexcept {
    written = 0;
    if (!valid_type31(preset, guard)) {
        return false;
    }
    return encode_type31_body(
        {preset.enabled, preset.generation, kUnusedAuxiliary}, output, written);
}

/** Encodes one complete type-31 body. */
bool encode_type31_body(const Type31Body& body,
                        std::span<std::byte> output,
                        std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kType31ByteCount) {
        return false;
    }
    bits::Writer writer(output.first(kType31ByteCount));
    const bool encoded = writer.write(body.enabled ? kEnabled : 0U, kBoolWidth)
                         && writer.write(body.value, kWideIntegerWidth)
                         && writer.write(body.auxiliary, kWideIntegerWidth)
                         && writer.bit_count() == kType31BitCount;
    return encoded && writer.finish(written) && written == kType31ByteCount;
}

/** Decodes one complete type-31 body. */
bool decode_type31_body(std::span<const std::byte> input, Type31Body& body) noexcept {
    if (input.size() != kType31ByteCount) {
        return false;
    }
    bits::Reader reader(input);
    std::uint64_t enabled = 0;
    Type31Body parsed{};
    if (!reader.read(kBoolWidth, enabled) || !reader.read(kWideIntegerWidth, parsed.value)
        || !reader.read(kWideIntegerWidth, parsed.auxiliary) || !finish_padding(reader)) {
        return false;
    }
    parsed.enabled = enabled != 0;
    body = parsed;
    return true;
}

/** Encodes one complete type-18 body. */
bool encode_type18_body(const Type18Body& body,
                        std::span<std::byte> output,
                        std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kType18ByteCount) {
        return false;
    }
    bits::Writer writer(output.first(kType18ByteCount));
    const std::uint32_t encodedValue = std::bit_cast<std::uint32_t>(body.tailValue) + kSigned32Bias;
    const bool encoded = write_shared_timed_state(writer, body.timed)
                         && writer.write(body.tailFlag ? 1U : 0U, kBoolWidth)
                         && writer.write(encodedValue, kSigned32Width)
                         && writer.bit_count() == kType18BitCount;
    return encoded && writer.finish(written) && written == kType18ByteCount;
}

/** Decodes one complete type-18 body. */
bool decode_type18_body(std::span<const std::byte> input, Type18Body& body) noexcept {
    if (input.size() != kType18ByteCount) {
        return false;
    }
    bits::Reader reader(input);
    Type18Body parsed{};
    std::uint64_t flag = 0;
    std::uint64_t encodedValue = 0;
    if (!read_shared_timed_state(reader, parsed.timed) || !reader.read(kBoolWidth, flag)
        || !reader.read(kSigned32Width, encodedValue) || !finish_padding(reader)) {
        return false;
    }
    parsed.tailFlag = flag != 0;
    parsed.tailValue =
        std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(encodedValue) - kSigned32Bias);
    body = parsed;
    return true;
}

/** Encodes one complete type-35 countdown/timer body. */
bool encode_type35_body(const Type35Body& body,
                        std::span<std::byte> output,
                        std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kType35ByteCount || !valid_mode(body.mode0) || !valid_mode(body.mode1)) {
        return false;
    }
    bits::Writer writer(output.first(kType35ByteCount));
    const bool encoded =
        writer.write(body.flag0 ? 1U : 0U, kBoolWidth)
        && writer.write(body.flag1 ? 1U : 0U, kBoolWidth)
        && writer.write(static_cast<std::uint32_t>(static_cast<std::int32_t>(body.mode0)
                                                   + static_cast<std::int32_t>(kModeBias)),
                        kModeWidth)
        && writer.write(static_cast<std::uint32_t>(static_cast<std::int32_t>(body.mode1)
                                                   + static_cast<std::int32_t>(kModeBias)),
                        kModeWidth)
        && write_shared_timed_state(writer, body.timed) && writer.bit_count() == kType35BitCount;
    return encoded && writer.finish(written) && written == kType35ByteCount;
}

/** Decodes one complete type-35 countdown/timer body. */
bool decode_type35_body(std::span<const std::byte> input, Type35Body& body) noexcept {
    if (input.size() != kType35ByteCount) {
        return false;
    }
    bits::Reader reader(input);
    Type35Body parsed{};
    std::uint64_t flag0 = 0;
    std::uint64_t flag1 = 0;
    std::uint64_t mode0 = 0;
    std::uint64_t mode1 = 0;
    if (!reader.read(kBoolWidth, flag0) || !reader.read(kBoolWidth, flag1)
        || !reader.read(kModeWidth, mode0) || !reader.read(kModeWidth, mode1)
        || !read_shared_timed_state(reader, parsed.timed) || !finish_padding(reader)) {
        return false;
    }
    parsed.flag0 = flag0 != 0;
    parsed.flag1 = flag1 != 0;
    parsed.mode0 = static_cast<std::int8_t>(static_cast<std::int32_t>(mode0)
                                            - static_cast<std::int32_t>(kModeBias));
    parsed.mode1 = static_cast<std::int8_t>(static_cast<std::int32_t>(mode1)
                                            - static_cast<std::int32_t>(kModeBias));
    body = parsed;
    return true;
}

/**
 * Encodes the counted Type-24 output rows without changing unselected row revisions.
 * @param body Rows in the target object's authored order.
 * @param output Receives the packed body.
 * @param written Receives the byte count, or zero on failure.
 * @param writtenBits Receives the bit count, or zero on failure.
 * @return False for invalid values, excessive rows or insufficient output space.
 */
bool encode_type24(const Type24Body& body,
                   std::span<std::byte> output,
                   std::size_t& written,
                   std::size_t& writtenBits) noexcept {
    written = 0;
    writtenBits = 0;
    if (body.count > body.channels.size()) {
        return false;
    }
    const auto channels = std::span(body.channels).first(body.count);
    for (const Type24Channel& channel : channels) {
        if (!std::isfinite(channel.value) || channel.value < kType24MinimumValue
            || channel.value > kType24MaximumValue || !std::isfinite(channel.blend)
            || channel.blend < 0.F) {
            return false;
        }
    }
    const std::size_t bits = kType24CountWidth + channels.size() * kType24ChannelBitCount;
    const std::size_t bytes = (bits + 7U) / 8U;
    if (output.size() < bytes) {
        return false;
    }
    encoding::bits::Writer writer(output.first(bytes));
    bool encoded = writer.write(body.count, kType24CountWidth);
    for (const Type24Channel& channel : channels) {
        encoded = encoded
                  && writer.write(std::bit_cast<std::uint32_t>(channel.revision) + kSigned32Bias,
                                  kSigned32Width)
                  && writer.write(std::bit_cast<std::uint32_t>(channel.value), kReal32Width)
                  && writer.write(std::bit_cast<std::uint32_t>(channel.blend), kReal32Width);
    }
    if (!encoded || !writer.finish(written)) {
        written = 0;
        return false;
    }
    writtenBits = bits;
    return true;
}

/** Decodes the populated channel prefix and requires exact bit length and zero padding. */
bool decode_type24(std::span<const std::byte> input,
                   std::size_t bitCount,
                   Type24Body& body) noexcept {
    if (bitCount < kType24CountWidth || bitCount > kType24MaximumBitCount
        || input.size() != (bitCount + 7U) / 8U) {
        return false;
    }
    bits::Reader reader(input);
    std::uint64_t count{};
    if (!reader.read(kType24CountWidth, count) || count > kType24ChannelCapacity
        || bitCount != kType24CountWidth + count * kType24ChannelBitCount) {
        return false;
    }
    Type24Body parsed{};
    parsed.count = static_cast<std::uint8_t>(count);
    for (Type24Channel& channel : std::span(parsed.channels).first(parsed.count)) {
        std::uint64_t revision{};
        std::uint64_t value{};
        std::uint64_t blend{};
        if (!reader.read(kSigned32Width, revision) || !reader.read(kReal32Width, value)
            || !reader.read(kReal32Width, blend)) {
            return false;
        }
        channel.revision =
            std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(revision) - kSigned32Bias);
        channel.value = std::bit_cast<float>(static_cast<std::uint32_t>(value));
        channel.blend = std::bit_cast<float>(static_cast<std::uint32_t>(blend));
        if (!std::isfinite(channel.value) || channel.value < kType24MinimumValue
            || channel.value > kType24MaximumValue || !std::isfinite(channel.blend)
            || channel.blend < 0.F) {
            return false;
        }
    }
    if (!finish_padding(reader)) {
        return false;
    }
    body = parsed;
    return true;
}

/** Encodes one Type-32 volume body: the volume's ClientRef, then its on or off state. */
bool encode_type32_volume(const Type32VolumeBody& body,
                          std::span<std::byte> output,
                          std::size_t& written) noexcept {
    written = 0;
    if (body.registryKey == 0 || body.registryKey == (std::numeric_limits<std::uint32_t>::max)()
        || body.slotIndex < 0 || output.size() < kType32ByteCount) {
        return false;
    }
    bits::Writer writer(output.first(kType32ByteCount));
    return auth_fields::write_client_ref(writer,
                                         body.registryKey,
                                         kVolumeSlotType,
                                         static_cast<std::uint16_t>(body.slotIndex))
           && writer.write(body.active ? kType32StateOn : kType32StateOff, kType32StateWidth)
           && auth_fields::finish_exact(writer, kType32BitCount, kType32ByteCount, written);
}

/** Decodes one volume toggle; the no-op state, a non-volume target and nonzero padding fail. */
bool decode_type32_volume(std::span<const std::byte> input,
                          std::size_t bitCount,
                          Type32VolumeBody& body) noexcept {
    if (bitCount != kType32BitCount || input.size() != kType32ByteCount) {
        return false;
    }
    bits::Reader reader(input);
    std::uint64_t key = 0, type = 0, index = 0, state = 0;
    if (!reader.read(auth_fields::kClientRefKeyWidth, key) || key == 0
        || key == (std::numeric_limits<std::uint32_t>::max)()
        || !reader.read(kClientRefTypeWidth, type)
        || type != kVolumeSlotType + auth_fields::kClientRefTypeBias
        || !reader.read(kClientRefIndexWidth, index) || index < kClientRefIndexBias
        || !reader.read(kType32StateWidth, state)
        || (state != kType32StateOff && state != kType32StateOn) || !finish_padding(reader)) {
        return false;
    }
    body = {static_cast<std::uint32_t>(key),
            static_cast<std::int16_t>(index - kClientRefIndexBias),
            state == kType32StateOn};
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::scriptable_auth
