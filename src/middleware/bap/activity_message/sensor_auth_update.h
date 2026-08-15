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
/** Groups one destination may publish. No installed destination reaches more than two. */
inline constexpr std::size_t kGroupCapacity = 4;
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
 * Slot indices are contiguous from zero, so a slot's ordinal in these arrays is its index.
 */
struct Group final {
    std::uint32_t key{};
    std::span<const std::uint8_t> slotTypes{};
    std::span<const std::uint8_t> slotFlags{};
};

/** Which groups one destination publishes and which of them binds the player. */
struct Roster final {
    std::array<Group, kGroupCapacity> groups{};
    std::size_t groupCount{};
    /** Group whose first type-13 block carries the player key. It must be one that registers. */
    std::uint32_t playerKeyGroup{};
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

/** @param keyCount Published group count. @return Total delta size from its own root bit. */
[[nodiscard]] constexpr std::size_t delta_bits(std::size_t keyCount) noexcept {
    return delta_state_count_bit(keyCount) + kDeltaCountWidth + 8 * keyCount + 1;
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
