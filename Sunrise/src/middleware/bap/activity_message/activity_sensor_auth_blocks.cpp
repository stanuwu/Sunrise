#include <algorithm>
#include <limits>

#include "../../encoding/bit_reader.h"
#include "roster_presence.h"
#include "scene_events_auth.h"
#include "sensor_auth_update.h"

namespace sunrise::middleware::bap::activity_message::sensor_auth_update {
namespace {

namespace bits = encoding::bits;

/** The widest chunk the bit writer accepts in one call. */
constexpr std::uint8_t kChunkWidth = 32;

/** The type-17 lifetime slot. The spawn gate reads whichever type-17 registers first, so every
 *  one must carry its state, even on a mission-seed placeholder group. */
constexpr std::uint8_t kSlotTypeLifetime = 17;

/** @return The override when this is its one exact object slot. */
[[nodiscard]] const AuthOverride* matching_override(const Snapshot& snapshot,
                                                    std::uint32_t objectTag,
                                                    std::uint32_t key,
                                                    std::uint8_t slotType,
                                                    std::uint16_t slotIndex) noexcept {
    for (const AuthOverride& value : snapshot.authOverrides) {
        if (value.present && value.objectTag == objectTag && value.key == key
            && value.slotType == slotType && value.slotIndex == slotIndex) {
            return &value;
        }
    }
    return nullptr;
}

/** Writes one MSB-first packed body without copying its padded final bits. */
template <typename PackedBody>
[[nodiscard]] bool write_packed(bits::Writer& writer, const PackedBody& value) noexcept {
    bool encoded = true;
    std::size_t remaining = value.bitCount;
    for (std::size_t index = 0; encoded && remaining != 0; ++index) {
        const std::uint8_t width = static_cast<std::uint8_t>((std::min)(remaining, std::size_t{8}));
        std::uint8_t byte = std::to_integer<std::uint8_t>(value.body[index]);
        if (width < 8) {
            byte = static_cast<std::uint8_t>(byte >> (8 - width));
        }
        encoded = writer.write(byte, width);
        remaining -= width;
    }
    return encoded;
}

} // namespace

/**
 * Checks the bounded scene dependency set before it enters the host queue.
 * @param value Dependency set.
 * @return True for bounded, unique references.
 */
bool valid_authored_scene_dependencies(const AuthoredSceneDependencies& value) noexcept {
    if (value.count > value.references.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.count; ++index) {
        const auto& ref = value.references[index];
        if (ref.rosterKey == 0 || ref.rosterKey == kEmptyNameHash
            || ref.rosterKey == (std::numeric_limits<std::uint32_t>::max)() || ref.slotType < 0
            || ref.slotType > kMaximumSlotType || ref.slotIndex < 0) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            const auto& other = value.references[prior];
            if (ref.rosterKey == other.rosterKey && ref.slotType == other.slotType
                && ref.slotIndex == other.slotIndex) {
                return false;
            }
        }
    }
    return true;
}

/**
 * Encodes the Type 43 generation and authored spawn dependencies.
 * @param generation Positive activation generation.
 * @param dependencies Exact spawn dependencies.
 * @param output Receives the packed Auth body.
 * @param written Receives the byte count, or zero on failure.
 * @param writtenBits Receives the bit count, or zero on failure.
 * @return True when every field fits its native width.
 */
bool encode_authored_scene_auth(std::uint32_t generation,
                                const AuthoredSceneDependencies& dependencies,
                                std::span<std::byte> output,
                                std::size_t& written,
                                std::size_t& writtenBits,
                                std::span<const std::uint32_t> events,
                                std::uint32_t scalar,
                                bool stop) noexcept {
    /** Native signed generation uses a 32-bit bias. */
    constexpr std::uint32_t generationBias = 0x80000000U;
    /** Native dependency count, scalar and event count widths. */
    constexpr std::uint8_t dependencyCountWidth = 4, scalarWidth = 31, eventCountWidth = 6;
    written = 0;
    writtenBits = 0;
    if (generation == 0 || generation >= generationBias
        || !valid_authored_scene_dependencies(dependencies)
        || events.size() > kAuthoredSceneMaximumEventCount || scalar >= generationBias) {
        return false;
    }
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto prior = events.first(index);
        if (events[index] == 0 || events[index] == scene_events::kInvalidEventKey
            || std::find(prior.begin(), prior.end(), events[index]) != prior.end()) {
            return false;
        }
    }
    bits::Writer writer(output);
    bool encoded = writer.write(generation + generationBias, kKeyWidth)
                   && writer.write(stop, kPresenceWidth)
                   && writer.write(dependencies.count, dependencyCountWidth);
    for (std::size_t index = 0; encoded && index < dependencies.count; ++index) {
        const auto& ref = dependencies.references[index];
        encoded = writer.write(ref.rosterKey, kKeyWidth)
                  && writer.write(static_cast<std::uint32_t>(ref.slotType) + kSlotTypeBias,
                                  kSlotTypeWidth)
                  && writer.write(static_cast<std::uint32_t>(ref.slotIndex) + kSlotIndexBias,
                                  kSlotIndexWidth);
    }
    encoded = encoded && writer.write(scalar, scalarWidth)
              && writer.write(events.size(), eventCountWidth);
    for (const std::uint32_t key : events) {
        encoded = encoded && writer.write(key, kAuthoredSceneEventBitCount);
    }
    const auto expected = kAuthoredSceneBaseAuthBitCount
                          + kAuthoredSceneDependencyBitCount * dependencies.count
                          + kAuthoredSceneEventBitCount * events.size();
    if (!encoded || writer.bit_count() != expected || !writer.finish(written)) {
        written = 0;
        return false;
    }
    writtenBits = expected;
    return true;
}

/** Adds an event or stops a complete active scene body without changing its generation. */
bool update_authored_scene_auth(std::span<const std::byte> previous,
                                std::size_t previousBits,
                                std::uint32_t eventKey,
                                bool stop,
                                std::span<std::byte> output,
                                std::size_t& written,
                                std::size_t& writtenBits,
                                std::uint32_t& generation) noexcept {
    written = 0;
    writtenBits = 0;
    generation = 0;
    if (previousBits < kAuthoredSceneBaseAuthBitCount
        || previousBits > kAuthoredSceneMaximumAuthBitCount
        || previous.size() != (previousBits + 7U) / 8U || (stop ? eventKey != 0 : eventKey == 0)
        || eventKey == scene_events::kInvalidEventKey) {
        return false;
    }
    bits::Reader reader(previous);
    std::uint64_t encodedGeneration = 0, clear = 0, count = 0, scalar = 0;
    if (!reader.read(kKeyWidth, encodedGeneration) || !reader.read(kPresenceWidth, clear)
        || clear != 0 || encodedGeneration <= auth_fields::kSigned32Bias
        || !reader.read(scene_events::kDependencyCountWidth, count)
        || count > kAuthoredSceneMaximumDependencyCount) {
        return false;
    }
    AuthoredSceneDependencies dependencies{};
    dependencies.count = static_cast<std::uint8_t>(count);
    for (std::size_t index = 0; index < dependencies.count; ++index) {
        std::uint64_t key = 0, type = 0, slot = 0;
        if (!reader.read(kKeyWidth, key) || !reader.read(kSlotTypeWidth, type)
            || !reader.read(kSlotIndexWidth, slot) || type < kSlotTypeBias
            || slot < kSlotIndexBias) {
            return false;
        }
        dependencies.references[index] = {static_cast<std::uint32_t>(key),
                                          static_cast<std::int8_t>(type - kSlotTypeBias),
                                          static_cast<std::int16_t>(slot - kSlotIndexBias)};
    }
    if (!valid_authored_scene_dependencies(dependencies)
        || !reader.read(scene_events::kScalarWidth, scalar)
        || !reader.read(scene_events::kEventCountWidth, count)
        || count > kAuthoredSceneMaximumEventCount
        || previousBits
               != kAuthoredSceneBaseAuthBitCount
                      + kAuthoredSceneDependencyBitCount * dependencies.count
                      + kAuthoredSceneEventBitCount * count) {
        return false;
    }
    std::array<std::uint32_t, kAuthoredSceneMaximumEventCount> events{};
    bool found = stop;
    for (std::size_t index = 0; index < count; ++index) {
        std::uint64_t key = 0;
        if (!reader.read(kAuthoredSceneEventBitCount, key) || key == 0
            || key == scene_events::kInvalidEventKey) {
            return false;
        }
        events[index] = static_cast<std::uint32_t>(key);
        const auto prior = std::span(events).first(index);
        if (std::find(prior.begin(), prior.end(), events[index]) != prior.end()) {
            return false;
        }
        found = found || events[index] == eventKey;
    }
    std::uint64_t padding = 0;
    if (!reader.read(static_cast<std::uint8_t>(previous.size() * 8U - previousBits), padding)
        || padding != 0 || (!found && count == events.size())) {
        return false;
    }
    if (!found) {
        events[count++] = eventKey;
    }
    const auto retainedGeneration =
        static_cast<std::uint32_t>(encodedGeneration) - auth_fields::kSigned32Bias;
    if (!encode_authored_scene_auth(retainedGeneration,
                                    dependencies,
                                    output,
                                    written,
                                    writtenBits,
                                    std::span(events).first(static_cast<std::size_t>(count)),
                                    static_cast<std::uint32_t>(scalar),
                                    stop)) {
        return false;
    }
    generation = retainedGeneration;
    return true;
}

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
 * Writes one fixed key presence mask, low bit first.
 * A key whose bit is clear is dropped in silence, so the mask has to match the key count exactly.
 * @param writer Body writer.
 * @param keyCount Keys the matching list carries.
 * @param wordCount Words in the schema's fixed mask.
 * @return True when the whole mask fits.
 */
[[nodiscard]] bool
write_key_mask(bits::Writer& writer, std::size_t keyCount, std::size_t wordCount) noexcept {
    if (keyCount > wordCount * kChunkWidth) {
        return false;
    }
    bool encoded = true;
    for (std::size_t word = 0; encoded && word < wordCount; ++word) {
        const std::size_t low = word * kChunkWidth;
        const std::size_t set = keyCount > low ? keyCount - low : 0;
        const std::size_t bits = set > kChunkWidth ? kChunkWidth : set;
        encoded = writer.write((std::uint64_t{1} << bits) - 1, kChunkWidth);
    }
    return encoded;
}

/** @return The revision owned by one roster key, or the snapshot-wide compatibility value. */
[[nodiscard]] std::uint8_t
group_state_sequence(const Roster& roster, std::uint32_t key, std::uint8_t fallback) noexcept {
    for (std::size_t index = 0; index < roster.groupCount; ++index) {
        const Group& group = roster.groups[index];
        if (group.key == key) {
            return group.hasStateSequence ? group.stateSequence : fallback;
        }
    }
    return fallback;
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
                                          const Roster& roster,
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
    encoded = encoded && writer.write(1, kPresenceWidth);
    for (std::size_t word = 0; encoded && word < kBubbleMaskWords; ++word) {
        encoded = writer.write(presence_word(roster, block.keys, word), kChunkWidth);
    }
    encoded = encoded && writer.write(1, kPresenceWidth) && writer.write(count, kBubbleCountWidth);
    for (std::size_t index = 0; encoded && index < keyCount; ++index) {
        encoded = writer.write(
            kStateByteBias + group_state_sequence(roster, block.keys[index], stateSequence), 8);
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
                                           const Roster& roster,
                                           std::uint8_t stateSequence) noexcept {
    const std::span<const BubbleSubBlock> subBlocks = roster.bubbleSubBlocks;
    bool encoded = writer.write(static_cast<std::uint32_t>(subBlocks.size()), kBubbleCountWidth);
    for (std::size_t index = 0; encoded && index < subBlocks.size(); ++index) {
        encoded = write_bubble_sub_block(writer, subBlocks[index], roster, stateSequence);
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
    encoded = encoded && write_key_mask(writer, keyCount, kDeltaMaskWords)
              && writer.write(1, kPresenceWidth)
              && writer.bit_count() == root + delta_state_count_bit(keyCount)
              && writer.write(static_cast<std::uint32_t>(keyCount), kDeltaCountWidth);
    for (std::size_t group = 0; encoded && group < keyCount; ++group) {
        const Group& row = roster.groups[group];
        encoded = writer.write(
            kStateByteBias + (row.hasStateSequence ? row.stateSequence : stateSequence), 8);
    }
    // Field 1 is the per-bubble sub-block half. Absent, it is one zero bit and 32 KB of the
    // client's roster struct stays untouched.
    const std::span<const BubbleSubBlock> subBlocks = roster.bubbleSubBlocks;
    encoded = encoded && writer.write(subBlocks.empty() ? 0U : 1U, kPresenceWidth);
    if (encoded && !subBlocks.empty()) {
        encoded = write_bubble_sub_blocks(writer, roster, stateSequence);
    }
    return encoded && writer.bit_count() == root + delta_bits(keyCount, subBlocks);
}

/** Writes one per-object state block. */
bool write_object_block(bits::Writer& writer,
                        const Snapshot& snapshot,
                        std::uint32_t objectTag,
                        std::uint32_t key,
                        std::uint8_t slotType,
                        std::uint16_t slotIndex,
                        std::uint8_t flags,
                        bool missionSeedOnly,
                        bool carriesPlayerKey) noexcept {
    const bool emitAuth = (flags & kSlotAuthFlag) != 0;
    const bool emitSense = (flags & kSlotSenseFlag) != 0;
    const AuthOverride* const override =
        emitAuth ? matching_override(snapshot, objectTag, key, slotType, slotIndex) : nullptr;
    const SenseOverride* sense = nullptr;
    if (emitSense) {
        for (const SenseOverride& candidate : snapshot.senseOverrides) {
            if (candidate.objectTag == objectTag && candidate.key == key
                && candidate.slotType == slotType && candidate.slotIndex == slotIndex) {
                sense = &candidate;
                break;
            }
        }
    }
    // A mission-seed group is a placeholder with no default body. The spawn gate reads two of
    // them: the player key (this type-13) and the lifetime state (every type-17, because the
    // gate reads whichever registers first). Zeroing those strands the spawn.
    const bool spawnBearing = carriesPlayerKey || slotType == kSlotTypeLifetime;
    const std::size_t body = override != nullptr
                                 ? override->bitCount
                                 : (emitAuth && (!missionSeedOnly || spawnBearing)
                                        ? auth_body_bits(snapshot, slotType, carriesPlayerKey)
                                        : 0);
    // Sense adds its state flag, reset bit, complete delta and 32-bit counter.
    const std::size_t senseBits = sense != nullptr ? 2U + sense->bitCount + kKeyWidth : 0U;
    const std::size_t remainder = (emitAuth ? 2U : 0U) + (emitSense ? 1U : 0U) + body + senseBits;
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
            encoded = override != nullptr
                          ? write_packed(writer, *override)
                          : write_auth_body(writer, snapshot, slotType, carriesPlayerKey);
        }
    }
    if (encoded && emitSense) {
        encoded = writer.write(sense != nullptr ? 1U : 0U, kPresenceWidth);
        if (encoded && sense != nullptr) {
            encoded = writer.write(0, kPresenceWidth) && writer.write(1, kPresenceWidth)
                      && write_packed(writer, *sense) && writer.write(sense->counter, kKeyWidth);
        }
    }
    return encoded && writer.bit_count() == start + remainder;
}

} // namespace sunrise::middleware::bap::activity_message::sensor_auth_update
