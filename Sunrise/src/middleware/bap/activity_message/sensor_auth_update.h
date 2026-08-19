#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_writer.h"
#include "activity_patch_epoch_parser.h"

namespace sunrise::middleware::bap::activity_message::sensor_auth_update {

/** The client roster and its bubble grants both use activity message type 5. */
inline constexpr std::uint32_t kMessageType = 5;
/** The authority table is 64 usable bubbles plus one fallback slot. */
inline constexpr std::size_t kAuthoritySlotCount = 65;
/** Bubble 64 is in the table but the world controller cannot enter it, so it is never granted. */
inline constexpr std::uint8_t kMaximumGrantBubble = 63;
/** A grant token of zero equals the client's cleared mirror, so it grants nothing. */
inline constexpr std::uint16_t kMinimumGrantToken = 1;
/**
 * Groups one body may carry, top-level and per-bubble together.
 * No installed destination reaches more than two objects that go in the top-level list, and the
 * per-bubble half adds its own. Phase 2 seeds every one of them.
 */
inline constexpr std::size_t kGroupCapacity = 8;
/** The three lifetime states spawn gate G4's unbounded jump table accepts. */
inline constexpr std::array<std::uint8_t, 3> kLifetimeStates = {3, 6, 10};
/** Slot flag bit for a block that carries a sense reset bit. */
inline constexpr std::uint8_t kSlotSenseFlag = 1;
/** Slot flag bit for a block that carries an auth reset bit and its delta root. */
inline constexpr std::uint8_t kSlotAuthFlag = 2;
/** The widest slice-set index the type-17 spawn override's bias-1 field accepts. */
inline constexpr std::uint32_t kMaximumSpawnSliceSet = 0x1FF;
/** The unset spawn-set hash. An override carrying it disables the override it was meant to arm. */
inline constexpr std::uint32_t kAbsentSpawnSetHash = 0x811C9DC5;

/** One bubble handed to this client, as a change against its own per-bubble mirror. */
struct Grant final {
    std::uint8_t bubble{};
    std::uint16_t token{};
};

/**
 * One roster group and its slots, in slot-index order.
 * A slot the object declares but no descriptor names is not here, so an ordinal is not an index
 * and `slotIndices` carries each slot's own, from its descriptor.
 */
struct Group final {
    std::uint32_t key{};
    std::span<const std::uint8_t> slotTypes{};
    std::span<const std::uint8_t> slotFlags{};
    std::span<const std::uint16_t> slotIndices{};
};

/** Sub-blocks the delta's field 1 may carry. The wire array declares one element per bubble. */
inline constexpr std::size_t kBubbleSubBlockCapacity = 64;
/** Keys one sub-block may carry. Its wire key array declares 96. */
inline constexpr std::size_t kBubbleKeyCapacity = 96;
/** The widest bubble a sub-block may name. The client masks the index it compares to six bits. */
inline constexpr std::uint32_t kMaximumSubBlockBubble = 63;

/**
 * One per-bubble roster sub-block, an element of the delta's field 1.
 * Its keys register only while that bubble is current, and the slice-set sweep deactivates them
 * through the same unchecked lookup, so a key here must be in every slice set of this bubble.
 */
struct BubbleSubBlock final {
    std::uint32_t bubble{};
    /** Keys registered while that bubble is current. Each one also needs a group to seed it. */
    std::span<const std::uint32_t> keys{};
};

/** Which groups one destination publishes and which of them binds the player. */
struct Roster final {
    /**
     * Every group this body registers, top-level first and per-bubble after.
     * Phase 2 seeds all of them: the client applies auth state only once every object registered
     * in the current bubble is seeded, so a group left out holds back the whole apply.
     */
    std::array<Group, kGroupCapacity> groups{};
    std::size_t groupCount{};
    /** Leading groups that go in the delta's top-level key list. The rest are per-bubble only. */
    std::size_t topLevelGroupCount{};
    /** Group whose first type-13 block carries the player key. It must be one that registers. */
    std::uint32_t playerKeyGroup{};
    /** Sub-blocks published in the delta's field 1. None leaves that field one zero bit. */
    std::span<const BubbleSubBlock> bubbleSubBlocks{};
};

/** Everything one `sensor_auth_update` carries. */
struct Snapshot final {
    /** Message 52's payload, echoed exactly. A wrong epoch skips phase 2 and reports nothing. */
    patch_epoch::PatchEpoch patchEpoch{};
    Roster roster{};
    Grant grant{};
    /** Message 12's member record key. Zero leaves every type-13 block inert. */
    std::uint64_t playerKey{};
    /** Per-entry state byte. A change tears down and rebuilds every roster-owned object. */
    std::uint8_t stateSequence{};
    /** The participation record's region index. Its `+8` latch needs it. */
    std::uint32_t region{};
    std::uint32_t spawnSetHash{};
    std::uint32_t spawnSliceSet{};
    std::uint8_t lifetime{};
    bool hasGrant{};
    bool hasRegion{};
    bool hasSpawnOverride{};
    /** Hold the client's spawn while it loads by emitting `awaiting_client_sync`. */
    bool awaitClientSync{};
    /** Register the groups and seed no object. Separates no components from no auth state. */
    bool phaseOneOnly{};
    /**
     * Fill the participation body on every type-13 slot, not only the group's first.
     * The gate reads the record of the object the player datum names. Only one type-13 slot
     * gets the body, so filling the first slot alone can miss that object.
     */
    bool keyOnEveryParticipationSlot{};
};

/**
 * Encodes one `sensor_auth_update` body.
 * Phase 2 has no resync point, so a one-bit slip corrupts every later block in silence. Every
 * writer checks its own end position and the encode fails rather than shipping a slipped body.
 * @param snapshot Patch epoch, optional bubble grant, and the destination's roster.
 * @param output Caller storage, left unchanged when validation fails or it is too small.
 * @param written Receives the encoded size on success or zero on failure.
 * @return True when the whole zero-padded body fits.
 */
[[nodiscard]] bool encode_sensor_auth_update(const Snapshot& snapshot,
                                             std::span<std::byte> output,
                                             std::size_t& written) noexcept;

/** Bits before the enable latch with no bubble block: 8 hardwipe, 128 epoch, 1 present, 64 token.
 */
inline constexpr std::size_t kLatchBitWithoutGrant = 201;
/** A bubble block adds the 65-bit authority mask, two head bits, three per element, and one token.
 */
inline constexpr std::size_t kBubbleBlockBits =
    kAuthoritySlotCount + 2 + 3 * kAuthoritySlotCount + 16;
/** Each patch-epoch element is an unsigned 64-bit wire value. */
inline constexpr std::uint8_t kEpochWidth = 64;
/** The unchecked hardwipe token is one byte, before the patch epoch. */
inline constexpr std::uint8_t kHardwipeWidth = 8;
/** The unchecked activity token follows the bubble block. */
inline constexpr std::uint8_t kActivityTokenWidth = 64;
/** Changed authority tokens use the schema's unsigned 16-bit field. */
inline constexpr std::uint8_t kGrantTokenWidth = 16;
/** Every presence bit and every loop continuation bit is one bit wide. */
inline constexpr std::uint8_t kPresenceWidth = 1;
/** Both roster delta counts use the same 9-bit field. */
inline constexpr std::uint8_t kDeltaCountWidth = 9;
/** The delta's key array starts here, measured from the delta's own root bit. */
inline constexpr std::size_t kDeltaKeysBit = 12;
/** The presence mask is eight words wide whatever the key count. */
inline constexpr std::size_t kDeltaMaskWords = 8;
/** A registry key and the per-object block's length field are both 32 bits. */
inline constexpr std::uint8_t kKeyWidth = 32;
/** The object reference is a bias-1 slot type and a bias-32768 slot index. */
inline constexpr std::uint8_t kSlotTypeWidth = 7;
inline constexpr std::uint8_t kSlotIndexWidth = 16;
inline constexpr std::uint32_t kSlotTypeBias = 1;
inline constexpr std::uint32_t kSlotIndexBias = 32768;
/** The widest slot index the biased 16-bit field carries. One above it wraps to zero. */
inline constexpr std::uint16_t kMaximumSlotIndex = 32767;
/** The per-entry state byte is stored biased, so the wire value never goes negative. */
inline constexpr std::uint32_t kStateByteBias = 0x80;

/** @param keyCount Published group count. @return Bit position of the delta's presence mask. */
[[nodiscard]] constexpr std::size_t delta_mask_bit(std::size_t keyCount) noexcept {
    return kDeltaKeysBit + 32 * keyCount + 1;
}

/** @param keyCount Published group count. @return Bit position of the delta's state count. */
[[nodiscard]] constexpr std::size_t delta_state_count_bit(std::size_t keyCount) noexcept {
    return delta_mask_bit(keyCount) + 32 * kDeltaMaskWords + 1;
}

/** The sub-block count and each nested count are 7-bit fields with no presence bit. */
inline constexpr std::uint8_t kBubbleCountWidth = 7;
/** The sub-block's own key is a signed 32-bit field, so its wire value carries the -2^31 bias. */
inline constexpr std::uint32_t kBubbleKeyBias = 0x80000000;
/** The sub-block presence mask is three words whatever the key count. */
inline constexpr std::size_t kBubbleMaskWords = 3;
/**
 * Bits one sub-block costs before its keys: the element's own presence bit, its bubble key, the
 * four remaining flags, both counts and the presence mask. These are the 5 flags per sub-block
 * that close the schema's 325-flag sum.
 */
inline constexpr std::size_t kBubbleSubBlockFixedBits =
    kPresenceWidth + kKeyWidth + kPresenceWidth + kPresenceWidth + kBubbleCountWidth
    + kPresenceWidth + 32 * kBubbleMaskWords + kPresenceWidth + kBubbleCountWidth;
/** Bits one key costs inside a sub-block: the key itself, then its state byte. */
inline constexpr std::size_t kBubbleSubBlockKeyBits = kKeyWidth + 8;

/**
 * @param subBlocks Sub-blocks the delta carries, which must not be empty.
 * @return Bits the whole field-1 half costs, after the field's own presence bit.
 */
[[nodiscard]] constexpr std::size_t
bubble_bits(std::span<const BubbleSubBlock> subBlocks) noexcept {
    std::size_t bits = kBubbleCountWidth;
    for (const BubbleSubBlock& block : subBlocks) {
        bits += kBubbleSubBlockFixedBits + kBubbleSubBlockKeyBits * block.keys.size();
    }
    return bits;
}

/**
 * @param keyCount Top-level group count.
 * @param subBlocks Sub-blocks the delta carries, empty when field 1 is absent.
 * @return Total delta size from its own root bit.
 */
[[nodiscard]] constexpr std::size_t delta_bits(std::size_t keyCount,
                                               std::span<const BubbleSubBlock> subBlocks) noexcept {
    return delta_state_count_bit(keyCount) + kDeltaCountWidth + 8 * keyCount + 1
           + (subBlocks.empty() ? 0 : bubble_bits(subBlocks));
}

/**
 * Writes zero bits in chunks the writer accepts.
 * @param writer Body writer.
 * @param count Bits to write.
 * @return True when every bit fits.
 */
[[nodiscard]] bool pad_bits(encoding::bits::Writer& writer, std::size_t count) noexcept;

/**
 * Writes the bubble authority block.
 * @param writer Body writer positioned after the block's present bit.
 * @param grant The one bubble to hand over.
 * @return True when the whole block fits.
 */
[[nodiscard]] bool write_bubble_block(encoding::bits::Writer& writer, const Grant& grant) noexcept;

/**
 * Writes the phase-1 roster delta, which registers the group keys.
 * @param writer Body writer positioned after the enable latch.
 * @param roster Groups to register, in publish order.
 * @param stateSequence Biased per-entry state byte.
 * @return True when the whole delta fits and lands on its own end bit.
 */
[[nodiscard]] bool write_roster_delta(encoding::bits::Writer& writer,
                                      const Roster& roster,
                                      std::uint8_t stateSequence) noexcept;

/**
 * Reports how many bits of auth body one slot carries.
 * @param snapshot Message input, which decides the type-13 body width.
 * @param slotType Slot type from the group's slot array.
 * @param carriesPlayerKey True for the one type-13 block that binds the player.
 * @return Body bits, or zero for a seed-only block.
 */
[[nodiscard]] std::size_t
auth_body_bits(const Snapshot& snapshot, std::uint8_t slotType, bool carriesPlayerKey) noexcept;

/**
 * Writes one slot's auth body.
 * @param writer Body writer positioned after the auth delta's root bit.
 * @param snapshot Message input.
 * @param slotType Slot type from the group's slot array.
 * @param carriesPlayerKey True for the one type-13 block that binds the player.
 * @return True when the body fits and matches its declared width.
 */
[[nodiscard]] bool write_auth_body(encoding::bits::Writer& writer,
                                   const Snapshot& snapshot,
                                   std::uint8_t slotType,
                                   bool carriesPlayerKey) noexcept;

/**
 * Writes one per-object state block.
 * The 32-bit length counts the remainder, which includes the reset bit, the auth root bit and the
 * sense bit. Leaving the reset bit out of it desyncs by 3 bits with nothing reported.
 * @param writer Body writer positioned at the block's continuation bit.
 * @param snapshot Message input.
 * @param key Registry key of the owning group.
 * @param slotType Slot type from the group's slot array.
 * @param slotIndex Slot ordinal, which is also the slot's index.
 * @param flags Sense and auth emit bits for that slot type.
 * @param carriesPlayerKey True for the one type-13 block that binds the player.
 * @return True when the block fits and lands on its declared end bit.
 */
[[nodiscard]] bool write_object_block(encoding::bits::Writer& writer,
                                      const Snapshot& snapshot,
                                      std::uint32_t key,
                                      std::uint8_t slotType,
                                      std::uint16_t slotIndex,
                                      std::uint8_t flags,
                                      bool carriesPlayerKey) noexcept;

} // namespace sunrise::middleware::bap::activity_message::sensor_auth_update
