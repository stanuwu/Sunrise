#include "sensor_auth_update.h"

namespace sunrise::middleware::bap::activity_message::sensor_auth_update {
namespace {

namespace bits = encoding::bits;

/** The widest chunk the bit writer accepts in one call. */
constexpr std::uint8_t kChunkWidth = 32;

} // namespace

/** Writes zero bits in chunks the writer accepts. */
bool pad_bits(bits::Writer& writer, std::size_t count) noexcept {
    bool encoded = true;
    for (std::size_t written = 0; encoded && written < count; written += kChunkWidth) {
        const std::size_t remaining = count - written;
        const auto width =
            static_cast<std::uint8_t>(remaining > kChunkWidth ? kChunkWidth : remaining);
        encoded = writer.write(0, width);
    }
    return encoded;
}

/** Writes the bubble authority block. */
bool write_bubble_block(bits::Writer& writer, const Grant& grant) noexcept {
    bool encoded = true;
    for (std::size_t bubble = 0; encoded && bubble < kAuthoritySlotCount; ++bubble) {
        encoded = writer.write(bubble == grant.bubble ? 1U : 0U, kPresenceWidth);
    }
    // The block's own root presence bit, then the absent i32 header at struct +0.
    encoded = encoded && writer.write(1, kPresenceWidth) && writer.write(0, kPresenceWidth);
    for (std::size_t bubble = 0; encoded && bubble < kAuthoritySlotCount; ++bubble) {
        const bool granted = bubble == grant.bubble;
        // The host token stays absent. A wire copy that differs from the mirror parks a 5 s stamp.
        encoded =
            writer.write(0, kPresenceWidth) && writer.write(granted ? 1U : 0U, kPresenceWidth);
        if (encoded && granted) {
            encoded = writer.write(grant.token, kGrantTokenWidth);
        }
        // The commit bool has no presence bit, so all 65 of them are explicit zeros.
        encoded = encoded && writer.write(0, kPresenceWidth);
    }
    return encoded;
}

namespace {

/**
 * Writes one sub-block's key presence mask, low bit first.
 * A key whose bit is clear is dropped in silence, so the mask has to match the key count exactly.
 * @param writer Body writer.
 * @param keyCount Keys the sub-block carries.
 * @return True when all three words fit.
 */
[[nodiscard]] bool write_key_mask(bits::Writer& writer, std::size_t keyCount) noexcept {
    bool encoded = true;
    for (std::size_t word = 0; encoded && word < kBubbleMaskWords; ++word) {
        const std::size_t low = word * kChunkWidth;
        const std::size_t set = keyCount > low ? keyCount - low : 0;
        const std::size_t bits = set > kChunkWidth ? kChunkWidth : set;
        encoded = writer.write((std::uint64_t{1} << bits) - 1, kChunkWidth);
    }
    return encoded;
}

/**
 * Writes one per-bubble sub-block: the bubble it belongs to, then its keys with their mask bits
 * and state bytes. ClientRoster_ApplyDelta applies only the sub-block whose key equals the current
 * bubble index, which is what makes these keys bubble-local.
 * @param writer Body writer.
 * @param block Bubble and keys to publish.
 * @param stateSequence Value each state byte carries, so a re-send can force a re-add.
 * @return True when the whole sub-block fits.
 */
[[nodiscard]] bool write_bubble_sub_block(bits::Writer& writer,
                                          const BubbleSubBlock& block,
                                          std::uint8_t stateSequence) noexcept {
    const std::size_t keyCount = block.keys.size();
    const auto count = static_cast<std::uint32_t>(keyCount);
    bool encoded = writer.write(1, kPresenceWidth)
                   && writer.write(kBubbleKeyBias + block.bubble, kKeyWidth)
                   && writer.write(1, kPresenceWidth) && writer.write(1, kPresenceWidth)
                   && writer.write(count, kBubbleCountWidth);
    for (std::size_t index = 0; encoded && index < keyCount; ++index) {
        encoded = writer.write(block.keys[index], kKeyWidth);
    }
    encoded = encoded && writer.write(1, kPresenceWidth) && write_key_mask(writer, keyCount)
              && writer.write(1, kPresenceWidth) && writer.write(count, kBubbleCountWidth);
    for (std::size_t index = 0; encoded && index < keyCount; ++index) {
        encoded = writer.write(kStateByteBias + stateSequence, 8);
    }
    return encoded;
}

/**
 * Writes the delta's field-1 half: the sub-block count, then one element each.
 * @param writer Body writer positioned after the field's presence bit.
 * @param subBlocks Sub-blocks to publish, in bubble order.
 * @param stateSequence Value each state byte carries.
 * @return True when every sub-block fits.
 */
[[nodiscard]] bool write_bubble_sub_blocks(bits::Writer& writer,
                                           std::span<const BubbleSubBlock> subBlocks,
                                           std::uint8_t stateSequence) noexcept {
    bool encoded = writer.write(static_cast<std::uint32_t>(subBlocks.size()), kBubbleCountWidth);
    for (std::size_t index = 0; encoded && index < subBlocks.size(); ++index) {
        encoded = write_bubble_sub_block(writer, subBlocks[index], stateSequence);
    }
    return encoded;
}

} // namespace

/** Writes the phase-1 roster delta, which registers the group keys. */
bool write_roster_delta(bits::Writer& writer,
                        const Roster& roster,
                        std::uint8_t stateSequence) noexcept {
    const std::size_t root = writer.bit_count();
    const std::size_t keyCount = roster.topLevelGroupCount;
    // Clearing the root presence bit means nothing below it is read.
    bool encoded = writer.write(1, kPresenceWidth) && writer.write(1, kPresenceWidth)
                   && writer.write(1, kPresenceWidth)
                   && writer.write(static_cast<std::uint32_t>(keyCount), kDeltaCountWidth)
                   && writer.bit_count() == root + kDeltaKeysBit;
    for (std::size_t group = 0; encoded && group < keyCount; ++group) {
        encoded = writer.write(roster.groups[group].key, kKeyWidth);
    }
    encoded = encoded && writer.write(1, kPresenceWidth)
              && writer.bit_count() == root + delta_mask_bit(keyCount);
    // A key whose mask bit is clear is dropped in silence, so the mask must match the key count.
    const std::uint32_t mask =
        keyCount == 0 ? 0U : static_cast<std::uint32_t>((std::uint64_t{1} << keyCount) - 1);
    encoded = encoded && writer.write(mask, kChunkWidth)
              && pad_bits(writer, kChunkWidth * (kDeltaMaskWords - 1))
              && writer.write(1, kPresenceWidth)
              && writer.bit_count() == root + delta_state_count_bit(keyCount)
              && writer.write(static_cast<std::uint32_t>(keyCount), kDeltaCountWidth);
    for (std::size_t group = 0; encoded && group < keyCount; ++group) {
        encoded = writer.write(kStateByteBias + stateSequence, 8);
    }
    // Field 1 is the per-bubble sub-block half. Absent, it is one zero bit and 32 KB of the
    // client's roster struct stays untouched.
    const std::span<const BubbleSubBlock> subBlocks = roster.bubbleSubBlocks;
    encoded = encoded && writer.write(subBlocks.empty() ? 0U : 1U, kPresenceWidth);
    if (encoded && !subBlocks.empty()) {
        encoded = write_bubble_sub_blocks(writer, subBlocks, stateSequence);
    }
    return encoded && writer.bit_count() == root + delta_bits(keyCount, subBlocks);
}

/** Writes one per-object state block. */
bool write_object_block(bits::Writer& writer,
                        const Snapshot& snapshot,
                        std::uint32_t key,
                        std::uint8_t slotType,
                        std::uint16_t slotIndex,
                        std::uint8_t flags,
                        bool carriesPlayerKey) noexcept {
    const bool emitAuth = (flags & kSlotAuthFlag) != 0;
    const bool emitSense = (flags & kSlotSenseFlag) != 0;
    const std::size_t body = emitAuth ? auth_body_bits(snapshot, slotType, carriesPlayerKey) : 0;
    const std::size_t remainder = (emitAuth ? 2U : 0U) + (emitSense ? 1U : 0U) + body;
    bool encoded = writer.write(1, kPresenceWidth) && writer.write(key, kKeyWidth)
                   && writer.write(std::uint32_t{slotType} + kSlotTypeBias, kSlotTypeWidth)
                   && writer.write(std::uint32_t{slotIndex} + kSlotIndexBias, kSlotIndexWidth)
                   && writer.write(static_cast<std::uint32_t>(remainder), kKeyWidth);
    const std::size_t start = writer.bit_count();
    if (encoded && emitAuth) {
        // A reset bit of zero on a first block decodes into a throwaway buffer and never seeds.
        encoded =
            writer.write(1, kPresenceWidth) && writer.write(body > 0 ? 1U : 0U, kPresenceWidth);
        if (encoded && body > 0) {
            encoded = write_auth_body(writer, snapshot, slotType, carriesPlayerKey);
        }
    }
    // A sense-present bit of one costs 35 more bits, not one, so it is always sent absent.
    if (encoded && emitSense) {
        encoded = writer.write(0, kPresenceWidth);
    }
    return encoded && writer.bit_count() == start + remainder;
}

} // namespace sunrise::middleware::bap::activity_message::sensor_auth_update
