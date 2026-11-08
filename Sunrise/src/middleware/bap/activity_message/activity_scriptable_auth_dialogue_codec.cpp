#include <algorithm>
#include <array>
#include <bit>
#include <limits>

#include "scriptable_auth_internal.h"

// The type-53 dialogue body. Each row fires once per new sequence, when its filter admits the
// local player, so a new body keeps the rows still waiting for their volume.

namespace sunrise::middleware::bap::activity_message::scriptable_auth {
namespace {

namespace bits = encoding::bits;

/** Values the client reads as a fired dialogue cue; anything else leaves it inactive. */
constexpr std::uint64_t kDialogueActiveWorld = 1;
constexpr std::int8_t kDialogueInactiveMode = -1;
constexpr std::int8_t kDialogueFireMode = 2;

/** @return True when the reference names a type-60 volume. */
[[nodiscard]] bool volume_filter(const Type2LaneClientRef& filter) noexcept {
    return filter.slotIndex >= 0 && filter.slotType == kType53FilterSlotType
           && filter.registryKey != kClientRefAbsentKey;
}

/** Reads one row's reference: absent, or a type-60 volume. */
[[nodiscard]] bool read_filter(bits::Reader& reader, Type2LaneClientRef& filter) noexcept {
    std::uint64_t key = 0;
    std::uint64_t type = 0;
    std::uint64_t index = 0;
    if (!reader.read(kSigned32Width, key) || !reader.read(kClientRefTypeWidth, type)
        || !reader.read(kClientRefIndexWidth, index)) {
        return false;
    }
    filter = {};
    if (key == kClientRefAbsentKey && type == 0 && index == kClientRefIndexBias - 1U) {
        return true;
    }
    if (type < auth_fields::kClientRefTypeBias || index < kClientRefIndexBias
        || index - kClientRefIndexBias
               > static_cast<std::uint64_t>((std::numeric_limits<std::int16_t>::max)())) {
        return false;
    }
    filter.registryKey = static_cast<std::uint32_t>(key);
    filter.slotType = static_cast<std::int8_t>(type - auth_fields::kClientRefTypeBias);
    filter.slotIndex = static_cast<std::int16_t>(index - kClientRefIndexBias);
    return volume_filter(filter);
}

} // namespace

/** Finds the next positive dialogue fire sequence without wrapping. */
bool next_type53_sequence(const Type53SequenceGuard& guard,
                          std::uint16_t cueIndex,
                          std::int32_t& next) noexcept {
    next = 0;
    if (cueIndex >= guard.last.size() || guard.last[cueIndex] < 0
        || guard.last[cueIndex] == (std::numeric_limits<std::int32_t>::max)()) {
        return false;
    }
    next = guard.last[cueIndex] + 1;
    return next > 0;
}

/**
 * Fires one cue on top of a transported body. Filtered rows of the base stay active with their
 * sequence, so a line still waiting for its volume keeps waiting and a played one stays latched.
 * An unfiltered row played on arrival, so it is dropped.
 */
bool compose_type53(const Type53Body& base,
                    const Type53Preset& preset,
                    const Type53SequenceGuard& guard,
                    Type53Body& body) noexcept {
    const bool filtered = preset.filter.slotIndex >= 0;
    if (preset.cueIndex >= kType53EntryCount || preset.sequence <= 0
        || preset.sequence <= guard.last[preset.cueIndex]
        || preset.sequence <= base.rows[preset.cueIndex].sequence
        || (filtered && !volume_filter(preset.filter))) {
        return false;
    }
    Type53Body composed{};
    for (std::size_t index = 0; index < kType53EntryCount; ++index) {
        const Type53Row& kept = base.rows[index];
        Type53Row& row = composed.rows[index];
        if (index == preset.cueIndex) {
            row = {true, preset.sequence, preset.filter};
        } else if (kept.active && volume_filter(kept.filter)) {
            row = kept;
        } else {
            row.sequence = (std::max)(kept.sequence, guard.last[index]);
        }
    }
    body = composed;
    return true;
}

/** Encodes one complete type-53 body; its length grows with the active rows. */
bool encode_type53_body(const Type53Body& body,
                        std::span<std::byte> output,
                        std::size_t& written,
                        std::size_t& bits) noexcept {
    written = 0;
    const auto active = std::count_if(
        body.rows.begin(), body.rows.end(), [](const Type53Row& row) { return row.active; });
    bits = kType53MinimumBitCount + kType53WorldBitCount * static_cast<std::size_t>(active);
    const std::size_t bytes = (bits + 7) / 8;
    if (output.size() < bytes) {
        return false;
    }
    bits::Writer writer(output.first(bytes));
    bool encoded = write_absent_client_ref(writer);
    for (std::size_t index = 0; encoded && index < kType53EntryCount; ++index) {
        const Type53Row& row = body.rows[index];
        const std::uint32_t wireSequence =
            std::bit_cast<std::uint32_t>(row.sequence) + kSigned32Bias;
        const std::int8_t mode = row.active ? kDialogueFireMode : kDialogueInactiveMode;
        const bool filtered = row.active && row.filter.slotIndex >= 0;
        encoded =
            writer.write(0, kWideIntegerWidth) && writer.write(row.active ? 1U : 0U, kBoolWidth)
            && (!row.active || writer.write(kDialogueActiveWorld, kWideIntegerWidth))
            && (filtered ? auth_fields::write_client_ref(
                               writer,
                               row.filter.registryKey,
                               static_cast<std::uint32_t>(row.filter.slotType),
                               static_cast<std::uint16_t>(row.filter.slotIndex))
                         : write_absent_client_ref(writer))
            && writer.write(wireSequence, kSigned32Width)
            && writer.write(static_cast<std::uint32_t>(static_cast<std::int32_t>(mode) + kModeBias),
                            kModeWidth);
    }
    return encoded && writer.bit_count() == bits && writer.finish(written) && written == bytes;
}

/** Encodes a complete body that fires one cue and leaves every other row inactive. */
bool encode_type53(const Type53Preset& preset,
                   const Type53SequenceGuard& guard,
                   std::span<std::byte> output,
                   std::size_t& written) noexcept {
    written = 0;
    Type53Body body{};
    std::size_t bits = 0;
    return output.size() >= kType53ByteCount && compose_type53({}, preset, guard, body)
           && encode_type53_body(body, output, written, bits) && bits == kType53BitCount;
}

/**
 * Decodes one type-53 body in the form this tree encodes.
 * @return False on any other form: a timer tick, an inactive row with a filter or a wrong mode.
 */
bool decode_type53_body(std::span<const std::byte> input,
                        std::size_t bitCount,
                        Type53Body& body) noexcept {
    if (bitCount < kType53MinimumBitCount || bitCount > kType53MaximumBitCount
        || input.size() != (bitCount + 7) / 8) {
        return false;
    }
    bits::Reader reader(input);
    if (!read_absent_client_ref(reader)) {
        return false;
    }
    Type53Body parsed{};
    for (auto& row : parsed.rows) {
        std::uint64_t tick = 0;
        std::uint64_t hasWorld = 0;
        std::uint64_t world = 0;
        std::uint64_t wireSequence = 0;
        std::uint64_t wireMode = 0;
        if (!reader.read(kWideIntegerWidth, tick) || !reader.read(kBoolWidth, hasWorld)
            || (hasWorld != 0 && !reader.read(kWideIntegerWidth, world))
            || !read_filter(reader, row.filter) || !reader.read(kSigned32Width, wireSequence)
            || !reader.read(kModeWidth, wireMode) || tick != 0) {
            return false;
        }
        row.sequence =
            std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(wireSequence) - kSigned32Bias);
        const std::int32_t mode =
            static_cast<std::int32_t>(wireMode) - static_cast<std::int32_t>(kModeBias);
        row.active = hasWorld != 0;
        const bool valid = row.active ? world == kDialogueActiveWorld && row.sequence > 0
                                            && mode == kDialogueFireMode
                                      : mode == kDialogueInactiveMode && row.filter.slotIndex < 0;
        if (!valid) {
            return false;
        }
    }
    // The present world ids decide the length, so it must match the given count.
    if (input.size() * 8 - reader.remaining_bits() != bitCount || !finish_padding(reader)) {
        return false;
    }
    body = parsed;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::scriptable_auth
