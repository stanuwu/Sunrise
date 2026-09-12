#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_writer.h"
#include "activity_patch_epoch_parser.h"
#include "definition.h"
#include "sensor_message.h"
#include "squad_auth_body.h"
#include "squad_sense_state.h"

namespace sunrise::middleware::bap::activity_message::sensor_auth_update {

/** The client roster and its bubble grants both use activity message type 5. */
inline constexpr std::uint32_t kMessageType = 5;
/** The authority table is 64 usable bubbles plus one fallback slot. */
inline constexpr std::size_t kAuthoritySlotCount = 65;
/** Bubble 64 is in the table but the world controller cannot enter it, so it is never granted. */
inline constexpr std::uint8_t kMaximumGrantBubble = 63;
/** A grant token of zero equals the client's cleared mirror, so it grants nothing. */
inline constexpr std::uint16_t kMinimumGrantToken = 1;
/** Keys the client's top-level roster arrays hold. The wire count is wider and must be clamped. */
inline constexpr std::size_t kTopLevelGroupCapacity = 256;
/**
 * Groups one host snapshot retains and publishes across the top-level and all bubble lists.
 * This bounds fixed host storage; only one bubble list is active in the client manager at a time.
 */
inline constexpr std::size_t kPublishedGroupCapacity = 256;
/** Registrations the downstream ClientRef manager holds for top-level plus one active bubble. */
inline constexpr std::size_t kClientGroupCapacity = 128;
/** ClientRef sync records top-level plus one active bubble may create in one manager. */
inline constexpr std::size_t kClientRecordCapacity = 3072;
/** Exact auth bodies one snapshot accepts, bounded by the widest authored roster object. */
inline constexpr std::size_t kAuthOverrideCapacity = 1280;
/** Authored scene sensors are slot type 43 and consume reflection schema 0x8080626B. */
inline constexpr std::uint8_t kAuthoredSceneSlotType = 43;
inline constexpr std::uint32_t kAuthoredSceneAuthSchema = 0x8080626B;
/** Fixed fields plus bounded dependency references and event keys in the complete Auth schema. */
inline constexpr std::size_t kAuthoredSceneBaseAuthBitCount = 74;
inline constexpr std::size_t kAuthoredSceneDependencyBitCount = 55;
inline constexpr std::size_t kAuthoredSceneEventBitCount = 32;
inline constexpr std::size_t kAuthoredSceneMaximumDependencyCount = 8;
inline constexpr std::size_t kAuthoredSceneMaximumEventCount = 32;
inline constexpr std::size_t kAuthoredSceneMaximumAuthBitCount =
    kAuthoredSceneBaseAuthBitCount
    + kAuthoredSceneDependencyBitCount * kAuthoredSceneMaximumDependencyCount
    + kAuthoredSceneEventBitCount * kAuthoredSceneMaximumEventCount;
/** That widest body padded to whole bytes. */
inline constexpr std::size_t kAuthoredSceneMaximumAuthByteCount =
    (kAuthoredSceneMaximumAuthBitCount + 7U) / 8U;
/** Complete baseline body: signed activation generation, clear bool, and empty collections. */
inline constexpr std::uint16_t kAuthoredSceneAuthBitCount = kAuthoredSceneBaseAuthBitCount;
inline constexpr std::uint8_t kAuthoredSceneAuthByteCount = (kAuthoredSceneAuthBitCount + 7U) / 8U;
/** Authored scene dependencies are full ClientRefs, retained before the activation is queued. */
struct AuthoredSceneDependencies final {
    std::array<sensor_message::ClientReference, kAuthoredSceneMaximumDependencyCount> references{};
    std::uint8_t count{};
};

/** Checks the native count and ClientRef ranges without changing the dependency set. */
[[nodiscard]] bool
valid_authored_scene_dependencies(const AuthoredSceneDependencies& value) noexcept;

/** Encodes one activation with its exact dependencies, scalar and cumulative event keys. */
[[nodiscard]] bool encode_authored_scene_auth(std::uint32_t generation,
                                              const AuthoredSceneDependencies& dependencies,
                                              std::span<std::byte> output,
                                              std::size_t& written,
                                              std::size_t& writtenBits,
                                              std::span<const std::uint32_t> events = {},
                                              std::uint32_t scalar = 0,
                                              bool stop = false) noexcept;

/** Updates event keys or stops a scene while retaining its complete transported body. */
[[nodiscard]] bool update_authored_scene_auth(std::span<const std::byte> previous,
                                              std::size_t previousBits,
                                              std::uint32_t eventKey,
                                              bool stop,
                                              std::span<std::byte> output,
                                              std::size_t& written,
                                              std::size_t& writtenBits,
                                              std::uint32_t& generation) noexcept;
/** The widest statically bounded Auth body is type 19, at 53,150 bits. */
inline constexpr std::size_t kAuthOverrideByteCapacity = (53'150U + 7U) / 8U;
/** Lifetime states that pass the player-spawn gate. */
inline constexpr std::array<std::uint8_t, 3> kSpawnGateLifetimeStates = {3, 6, 10};
/** Highest lifetime state inside the client's jump table. */
inline constexpr std::uint8_t kMaximumLifetimeState = 10;
/** The lifetime field stores its value plus one in four bits. */
inline constexpr std::uint8_t kLifetimeWidth = 4;
inline constexpr std::uint32_t kLifetimeBias = 1;
/** Slot flag bit for a block that carries a sense reset bit. */
inline constexpr std::uint8_t kSlotSenseFlag = 1;
/** Slot flag bit for a block that carries an auth reset bit and its delta root. */
inline constexpr std::uint8_t kSlotAuthFlag = 2;
/** The widest slice-set index the type-17 spawn override's bias-1 field accepts. */
inline constexpr std::uint32_t kMaximumSpawnSliceSet = 0x1FF;
/** The unset spawn-set hash. An override carrying it disables the override it was meant to arm. */
inline constexpr std::uint32_t kAbsentSpawnSetHash = kEmptyNameHash;

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
    /** Package object tag retained off-wire for exact override target validation. */
    std::uint32_t objectTag{};
    std::uint32_t key{};
    std::span<const std::uint8_t> slotTypes{};
    std::span<const std::uint8_t> slotFlags{};
    std::span<const std::uint16_t> slotIndices{};
    /**
     * Revision for this one registry key. The wire carries one byte beside every key, so an
     * unrelated group's revision must not tear this group down and rebuild it.
     */
    std::uint8_t stateSequence{};
    /** False keeps hand-built callers on Snapshot::stateSequence for compatibility. */
    bool hasStateSequence{};
    /** True only for a generated mission group whose non-overridden slots seed empty deltas. */
    bool missionSeedOnly{};
    /** Retain this key's ordinal but clear its presence and omit its authority bodies. */
    bool retired{};
};

/** One exact, already-registered slot body substituted into phase 2. */
struct AuthOverride final {
    std::array<std::byte, kAuthOverrideByteCapacity> body{};
    std::uint32_t objectTag{};
    std::uint32_t key{};
    std::uint32_t authSchema{};
    std::uint16_t slotIndex{};
    std::uint16_t bitCount{};
    std::uint8_t slotType{};
    std::uint16_t byteCount{};
    /** Internal provenance: this body was compiled and revalidated against the pinned SDK. */
    bool sdkCompiled{};
    bool present{};
};

/** A full squad Sense baseline, including its delta root bit. */
struct SenseOverride final {
    std::array<std::byte, squad_sense::kMaximumByteCount> body{};
    std::uint32_t objectTag{};
    std::uint32_t key{};
    std::uint32_t counter{};
    std::size_t bitCount{};
    std::size_t byteCount{};
    std::uint16_t slotIndex{};
    std::uint8_t slotType{};
};

/** Sub-blocks the delta's field 1 may carry. The wire array declares one element per bubble. */
inline constexpr std::size_t kBubbleSubBlockCapacity = 64;
/** Keys one sub-block may carry. Its wire key array declares 96. */
inline constexpr std::size_t kBubbleKeyCapacity = 96;
/** The widest bubble a sub-block may name. The client masks the index it compares to six bits. */
inline constexpr std::uint32_t kMaximumSubBlockBubble = 63;

/**
 * One per-bubble roster sub-block, an element of the delta's field 1. Its keys register only
 * while that bubble is current, so a normal key exists in every selectable state.
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
     * Phase 2 seeds all of them. The client applies auth state only once every object registered
     * in the current bubble is seeded, so a group left out holds back the whole apply.
     */
    std::array<Group, kPublishedGroupCapacity> groups{};
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
    /** Exact typed bodies for slots already present in `roster`. */
    std::span<const AuthOverride> authOverrides{};
    std::span<const SenseOverride> senseOverrides{};
    Grant grant{};
    /** Message 12's member record key. Zero leaves every type-13 block inert. */
    std::uint64_t playerKey{};
    /** Compatibility revision used by groups without an explicit per-key value. */
    std::uint8_t stateSequence{};
    /** The participation record's region index. Its `+8` latch needs it. */
    std::uint32_t region{};
    std::uint32_t spawnSetHash{};
    std::uint32_t spawnSliceSet{};
    std::uint8_t lifetime{};
    bool hasGrant{};
    bool hasRegion{};
    bool hasSpawnOverride{};
    /** Set when the estate carries a darkness sensor body; the lifetime then names the bubble. */
    bool hasDarknessPolicy{};
    bool darknessEnabled{};
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

/** Bits before the enable latch, no bubble block: 8 hardwipe, 128 epoch, 1 present, 64 token. */
inline constexpr std::size_t kLatchBitWithoutGrant = 201;
/** A bubble block adds the 65-bit authority mask, two head bits, three per element, one token. */
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
/** Widest unbiased slot type carried by the bias-1 7-bit field; type zero is valid. */
inline constexpr std::uint8_t kMaximumSlotType =
    static_cast<std::uint8_t>(((std::uint32_t{1} << kSlotTypeWidth) - 1U) - kSlotTypeBias);
/** The widest slot index the biased 16-bit field carries. One above it wraps to zero. */
inline constexpr std::uint16_t kMaximumSlotIndex = 32767;
/**
 * Slot types whose Auth carries a lane that faults the stock client from a wire-legal value.
 * Only value, index and resolve lanes are listed. A count lane needs no entry, because the schema
 * codec caps every dynamic count at the nested schema's declared array length.
 */
inline constexpr std::array<std::uint8_t, 5> kWireFaultSlotTypes = {1, 8, 17, 38, 68};

/**
 * @param slotType Slot type an Auth override names.
 * @return True when no schema-compiled Auth body may carry that slot type.
 */
[[nodiscard]] constexpr bool refused_compiled_slot_type(std::uint8_t slotType) noexcept {
    for (const std::uint8_t refused : kWireFaultSlotTypes) {
        if (refused == slotType) {
            return true;
        }
    }
    return false;
}

/** Elements in the slot-type-1 requested-count array. Its wire count lane is wider. */
inline constexpr std::size_t kSquadRequestedCountArrayLength = 8;

/** The per-entry state byte is stored biased, so the wire value never goes negative. */
inline constexpr std::uint32_t kStateByteBias = 0x80;
/** The biased state byte wraps above this value. */
inline constexpr std::uint8_t kMaximumStateSequence = 127;

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
 * @param objectTag Package object tag of the owning group.
 * @param key Registry key of the owning group.
 * @param slotType Slot type from the group's slot array.
 * @param slotIndex Slot ordinal, which is also the slot's index.
 * @param flags Sense and auth emit bits for that slot type.
 * @param missionSeedOnly True to suppress the authored typed body unless an override matches.
 * @param carriesPlayerKey True for the one type-13 block that binds the player.
 * @return True when the block fits and lands on its declared end bit.
 */
[[nodiscard]] bool write_object_block(encoding::bits::Writer& writer,
                                      const Snapshot& snapshot,
                                      std::uint32_t objectTag,
                                      std::uint32_t key,
                                      std::uint8_t slotType,
                                      std::uint16_t slotIndex,
                                      std::uint8_t flags,
                                      bool missionSeedOnly,
                                      bool carriesPlayerKey) noexcept;

} // namespace sunrise::middleware::bap::activity_message::sensor_auth_update
